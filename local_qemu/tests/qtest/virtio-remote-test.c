/*
 * QTest testcase for virtio-remote
 *
 * Verifies the end-to-end path between a local QEMU presenting a
 * virtio-blk device and a remote stub process that owns the actual
 * block backend:
 *
 *   local: -device virtio-blk-pci,remote-machine=127.0.0.1@PORT,drive=drive0
 *   stub : remote-stub -device virtio-blk-pci,remote-stub=127.0.0.1@PORT,drive=drive0
 *
 * The stub process is the same QEMU binary reached through a symlink whose
 * name contains "remote-stub" (this is how main() selects the stub mode).
 * The test drives the guest virtqueue through libqos, writes and reads back
 * a sector, and then checks that the data landed on the stub's disk while
 * the local drive stayed untouched.
 *
 * Copyright (c) 2026
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "libqtest-single.h"
#include "qemu/module.h"
#include "standard-headers/linux/virtio_blk.h"
#include "libqos/malloc-pc.h"
#include "libqos/pci-pc.h"
#include "libqos/virtio-pci.h"

#define TIMEOUT_US              (30 * 1000 * 1000)
#define IMAGE_SIZE              (64 * 1024)
#define SECTOR_SIZE             512
#define PCI_SLOT                0x04
#define PCI_FN                  0x00

typedef struct Stub {
    GPid pid;
    gchar *tmpdir;
} Stub;

/* ask the kernel for a free TCP port. The stub binds it right after startup;
 * the local side retries its connect() for a while, so the tiny race here is
 * acceptable. */
static int get_free_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    g_assert_cmpint(fd, >=, 0);

    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    g_assert_cmpint(bind(fd, (struct sockaddr *)&addr, sizeof(addr)), ==, 0);
    socklen_t len = sizeof(addr);
    g_assert_cmpint(getsockname(fd, (struct sockaddr *)&addr, &len), ==, 0);
    int port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

static void create_image(const char *path, size_t size)
{
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, size), ==, 0);
    close(fd);
}

static void read_file_bytes(const char *path, void *buf, size_t len, off_t off)
{
    int fd = open(path, O_RDONLY);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pread(fd, buf, len, off), ==, (ssize_t)len);
    close(fd);
}

/* Spawn the remote stub process: a QEMU binary whose argv[0] contains
 * "remote-stub" (created via a symlink in a private tmp dir), running the
 * minimal x-remote-machine and owning the real block backend. */
static Stub *stub_start(int port, const char *img_path)
{
    const char *qemu_bin = qtest_qemu_binary(NULL);
    g_autofree gchar *abs_bin = g_canonicalize_filename(qemu_bin, NULL);

    Stub *stub = g_new0(Stub, 1);
    stub->tmpdir = g_dir_make_tmp("virtio-remote-stub-XXXXXX", NULL);
    g_autofree gchar *stub_path = g_strdup_printf("%s/remote-stub",
                                                  stub->tmpdir);

    /* main() switches to stub mode based on the program name */
    g_assert_cmpint(symlink(abs_bin, stub_path), ==, 0);

    g_autofree gchar *ip_port = g_strdup_printf("127.0.0.1@%d", port);
    g_autofree gchar *dev_opts = g_strdup_printf(
        "virtio-blk-pci,remote-stub=%s,drive=drive0", ip_port);
    g_autofree gchar *drive_opts = g_strdup_printf(
        "if=none,id=drive0,file=%s,format=raw,cache=writethrough", img_path);

    char *argv[] = {
        g_strdup(stub_path),
        g_strdup("-nodefaults"),
        g_strdup("-device"),
        g_strdup(dev_opts),
        g_strdup("-drive"),
        g_strdup(drive_opts),
        NULL
    };

    GError *err = NULL;
    gboolean ok = g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT,
                                NULL, NULL, &stub->pid, &err);
    for (int i = 0; argv[i]; i++) {
        g_free(argv[i]);
    }
    if (!ok) {
        g_error("failed to spawn remote stub: %s", err->message);
    }
    return stub;
}

/* The stub must have reached its main loop (and be listening) before the
 * local side connects. Polling the socket would consume the stub's single
 * control connection, so instead just wait for it to either exit early
 * (fail fast) or survive the grace period. */
static void stub_wait_ready(const Stub *stub)
{
    gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    while (g_get_monotonic_time() < deadline) {
        int wstatus;
        pid_t r = waitpid(stub->pid, &wstatus, WNOHANG);
        if (r == stub->pid) {
            if (WIFEXITED(wstatus)) {
                g_error("remote stub exited early with status %d",
                        WEXITSTATUS(wstatus));
            }
            g_error("remote stub died before the local side connected");
        }
        g_usleep(50 * 1000);
    }
}

static void stub_stop(Stub *stub)
{
    g_assert_cmpint(kill(stub->pid, SIGTERM), ==, 0);
    waitpid(stub->pid, NULL, 0);
    g_rmdir(stub->tmpdir);
    g_free(stub->tmpdir);
    g_free(stub);
}

/* Submit one virtio-blk request (16-byte header + 512-byte sector + status)
 * through the guest virtqueue and return the completion status byte. For a
 * read (VIRTIO_BLK_T_IN) the sector data is filled by the device. */
static uint8_t blk_req(QVirtioDevice *dev, QVirtQueue *vq,
                       QGuestAllocator *alloc, uint32_t type, uint64_t sector,
                       uint8_t *data)
{
    QTestState *qts = global_qtest;
    uint64_t req_addr = guest_alloc(alloc, 16 + SECTOR_SIZE + 1);
    uint32_t ioprio = 1;
    uint8_t status = 0xff;

    /* header (host-endian; the test host is little-endian x86) */
    qtest_memwrite(qts, req_addr, &type, sizeof(type));
    qtest_memwrite(qts, req_addr + 4, &ioprio, sizeof(ioprio));
    qtest_memwrite(qts, req_addr + 8, &sector, sizeof(sector));
    qtest_memwrite(qts, req_addr + 16, data, SECTOR_SIZE);
    qtest_memwrite(qts, req_addr + 16 + SECTOR_SIZE, &status, sizeof(status));

    uint32_t free_head = qvirtqueue_add(qts, vq, req_addr, 16, false, true);
    qvirtqueue_add(qts, vq, req_addr + 16, SECTOR_SIZE,
                   type == VIRTIO_BLK_T_IN, true);
    qvirtqueue_add(qts, vq, req_addr + 16 + SECTOR_SIZE, 1, true, false);
    qvirtqueue_kick(qts, dev, vq, free_head);

    qvirtio_wait_used_elem(qts, dev, vq, free_head, NULL, TIMEOUT_US);
    status = qtest_readb(qts, req_addr + 16 + SECTOR_SIZE);
    guest_free(alloc, req_addr);
    return status;
}

static void test_remote_roundtrip(void)
{
    int port = get_free_port();
    g_autofree gchar *tmpdir = g_dir_make_tmp("virtio-remote-test-XXXXXX",
                                              NULL);
    g_autofree gchar *local_img = g_strdup_printf("%s/local.img", tmpdir);
    g_autofree gchar *stub_img = g_strdup_printf("%s/stub.img", tmpdir);

    create_image(local_img, IMAGE_SIZE);
    create_image(stub_img, IMAGE_SIZE);

    Stub *stub = stub_start(port, stub_img);
    stub_wait_ready(stub);

    QTestState *qts = qtest_initf(
        "-nodefaults -M pc "
        "-drive if=none,id=drive0,file=%s,format=raw "
        "-device virtio-blk-pci,id=vblk,addr=0x%x,drive=drive0,"
        "remote-machine=127.0.0.1@%d",
        local_img, PCI_SLOT, port);
    global_qtest = qts;

    QGuestAllocator alloc;
    pc_alloc_init(&alloc, qts, ALLOC_NO_FLAGS);

    QPCIBusPC pcibus;
    qpci_init_pc(&pcibus, qts, &alloc);

    QVirtioPCIDevice vdev;
    QPCIAddress addr = { .devfn = QPCI_DEVFN(PCI_SLOT, PCI_FN) };
    virtio_pci_init(&vdev, &pcibus.bus, &addr);
    g_assert_cmphex(vdev.vdev.device_type, ==, VIRTIO_ID_BLOCK);
    qvirtio_pci_device_enable(&vdev);
    qvirtio_start_device(&vdev.vdev);

    uint64_t features = qvirtio_get_features(&vdev.vdev);
    features &= ~(QVIRTIO_F_BAD_FEATURE |
                  (1u << VIRTIO_RING_F_INDIRECT_DESC) |
                  (1u << VIRTIO_RING_F_EVENT_IDX) |
                  (1u << VIRTIO_BLK_F_SCSI));
    qvirtio_set_features(&vdev.vdev, features);

    QVirtQueue *vq = qvirtqueue_setup(&vdev.vdev, &alloc, 0);
    qvirtio_set_driver_ok(&vdev.vdev);

    /* write a sector through the remote path */
    uint8_t wdata[SECTOR_SIZE] = { 0 };
    static const char marker[] = "hello-remote-virtio";
    memcpy(wdata, marker, sizeof(marker) - 1);
    g_assert_cmpint(blk_req(&vdev.vdev, vq, &alloc,
                            VIRTIO_BLK_T_OUT, 0, wdata), ==, 0);

    /* read it back through the remote path */
    uint8_t rdata[SECTOR_SIZE] = { 0 };
    g_assert_cmpint(blk_req(&vdev.vdev, vq, &alloc,
                            VIRTIO_BLK_T_IN, 0, rdata), ==, 0);
    g_assert_cmpmem(rdata, SECTOR_SIZE, wdata, SECTOR_SIZE);

    /* the stub backend must have received the write */
    uint8_t stub_disk[SECTOR_SIZE] = { 0 };
    read_file_bytes(stub_img, stub_disk, sizeof(stub_disk), 0);
    g_assert_cmpmem(stub_disk, SECTOR_SIZE, wdata, SECTOR_SIZE);

    /* and the local drive must have been left untouched */
    uint8_t local_disk[SECTOR_SIZE] = { 0 };
    uint8_t zeros[SECTOR_SIZE] = { 0 };
    read_file_bytes(local_img, local_disk, sizeof(local_disk), 0);
    g_assert_cmpmem(local_disk, SECTOR_SIZE, zeros, SECTOR_SIZE);

    qvirtqueue_cleanup(vdev.vdev.bus, vq, &alloc);
    qvirtio_pci_device_disable(&vdev);
    alloc_destroy(&alloc);
    qtest_quit(qts);

    stub_stop(stub);
    g_rmdir(tmpdir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/virtio-remote/roundtrip", test_remote_roundtrip);
    return g_test_run();
}
