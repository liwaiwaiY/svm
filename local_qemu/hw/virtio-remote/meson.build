virtio_remote_ss = ss.source_set()
virtio_remote_ss.add(files('virtio-remote.c'))
virtio_remote_ss.add(dependency('liburing'))
system_ss.add_all(when: 'CONFIG_VIRTIO', if_true: virtio_remote_ss)
