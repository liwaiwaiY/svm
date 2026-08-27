---
language:
- en
tags:
- higgs
- tabular_classification
- binary_classification
- UCI
pretty_name: Higgs
size_categories:
- 10K<n<100K
task_categories:
- tabular-classification
configs:
- higgs
license: cc
---
# Higgs
The [Higgs dataset](https://www.nature.com/articles/ncomms5308/) from "[Searching for exotic particles in high-energy physics with deep learning](https://www.nature.com/articles/ncomms5308/)".
Try to classify particles as Higgs bosons.

# Configurations and tasks
| **Configuration** | **Task**                  | **Description**                                                 |
|-------------------|---------------------------|-----------------------------------------------------------------|
| higgs             | Binary classification     | Is the particle a Higgs boson?                                  |

# Usage
```python
from datasets import load_dataset

dataset = load_dataset("mstz/higgs")["train"]
```

# Features
|**Feature**                |**Type**   |
|---------------------------|-----------|
|`lepton_pT`                |`[float64]`|
|`lepton_eta`               |`[float64]`|
|`lepton_phi`               |`[float64]`|
|`missing_energy_magnitude` |`[float64]`|
|`missing_energy_phi`       |`[float64]`|
|`jet1pt`                   |`[float64]`|
|`jet1eta`                  |`[float64]`|
|`jet1phi`                  |`[float64]`|
|`jet1b`                    |`[float64]`|
|`jet2pt`                   |`[float64]`|
|`jet2eta`                  |`[float64]`|
|`jet2phi`                  |`[float64]`|
|`jet2b`                    |`[float64]`|
|`jet3pt`                   |`[float64]`|
|`jet3eta`                  |`[float64]`|
|`jet3phi`                  |`[float64]`|
|`jet3b`                    |`[float64]`|
|`jet4pt`                   |`[float64]`|
|`jet4eta`                  |`[float64]`|
|`jet4phi`                  |`[float64]`|
|`jet4b`                    |`[float64]`|
|`m_jj`                     |`[float64]`|
|`m_jjj`                    |`[float64]`|
|`m_lv`                     |`[float64]`|
|`m_jlv`                    |`[float64]`|
|`m_bb`                     |`[float64]`|
|`m_wbb`                    |`[float64]`|
|`m_wwbb`                   |`[float64]`|