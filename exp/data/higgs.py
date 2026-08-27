"""Higgs."""

from typing import List

import datasets

import pandas


VERSION = datasets.Version("1.0.0")
_ORIGINAL_FEATURE_NAMES = [
    "is_boson",
    "lepton_pT",
    "lepton_eta",
    "lepton_phi",
    "missing_energy_magnitude",
    "missing_energy_phi",
    "jet1pt",
    "jet1eta",
    "jet1phi",
    "jet1b-tag",
    "jet2pt",
    "jet2eta",
    "jet2phi",
    "jet2b-tag",
    "jet3pt",
    "jet3eta",
    "jet3phi",
    "jet3b-tag",
    "jet4pt",
    "jet4eta",
    "jet4phi",
    "jet4b-tag",
    "m_jj",
    "m_jjj",
    "m_lv",
    "m_jlv",
    "m_bb",
    "m_wbb",
    "m_wwbb"
]

DESCRIPTION = "Higgs dataset from \"Searching for exotic particles in high-energy physics with deep learning\"."
_HOMEPAGE = "https://www.nature.com/articles/ncomms5308/"
_URLS = ("https://www.openml.org/search?type=data&status=active&id=4532")
_CITATION = """
@article{baldi2014searching,
  title={Searching for exotic particles in high-energy physics with deep learning},
  author={Baldi, Pierre and Sadowski, Peter and Whiteson, Daniel},
  journal={Nature communications},
  volume={5},
  number={1},
  pages={4308},
  year={2014},
  publisher={Nature Publishing Group UK London}
}"""

# Dataset info
urls_per_split = {
    "train": "https://gist.githubusercontent.com/msetzu/99114313deb9cc98318d5940fd536b06/raw/7d4cb798cf460aa240b6b92d971009ae6790e3d0/gistfile1.txt",
}
features_types_per_config = {
    "higgs": {
        "lepton_pT": datasets.Value("float64"),
        "lepton_eta": datasets.Value("float64"),
        "lepton_phi": datasets.Value("float64"),
        "missing_energy_magnitude": datasets.Value("float64"),
        "missing_energy_phi": datasets.Value("float64"),
        "jet1pt": datasets.Value("float64"),
        "jet1eta": datasets.Value("float64"),
        "jet1phi": datasets.Value("float64"),
        "jet1b-tag": datasets.Value("float64"),
        "jet2pt": datasets.Value("float64"),
        "jet2eta": datasets.Value("float64"),
        "jet2phi": datasets.Value("float64"),
        "jet2b-tag": datasets.Value("float64"),
        "jet3pt": datasets.Value("float64"),
        "jet3eta": datasets.Value("float64"),
        "jet3phi": datasets.Value("float64"),
        "jet3b-tag": datasets.Value("float64"),
        "jet4pt": datasets.Value("float64"),
        "jet4eta": datasets.Value("float64"),
        "jet4phi": datasets.Value("float64"),
        "jet4b-tag": datasets.Value("float64"),
        "m_jj": datasets.Value("float64"),
        "m_jjj": datasets.Value("float64"),
        "m_lv": datasets.Value("float64"),
        "m_jlv": datasets.Value("float64"),
        "m_bb": datasets.Value("float64"),
        "m_wbb": datasets.Value("float64"),
        "m_wwbb": datasets.Value("float64"),
        "is_boson": datasets.ClassLabel(num_classes=2, names=("no", "yes"))
    }
}
features_per_config = {k: datasets.Features(features_types_per_config[k]) for k in features_types_per_config}


class HiggsConfig(datasets.BuilderConfig):
    def __init__(self, **kwargs):
        super(HiggsConfig, self).__init__(version=VERSION, **kwargs)
        self.features = features_per_config[kwargs["name"]]


class Higgs(datasets.GeneratorBasedBuilder):
    # dataset versions
    DEFAULT_CONFIG = "higgs"
    BUILDER_CONFIGS = [
        HiggsConfig(name="higgs",
                    description="Higgs boson binary classification.")
    ]


    def _info(self):
        info = datasets.DatasetInfo(description=DESCRIPTION, citation=_CITATION, homepage=_HOMEPAGE,
                                    features=features_per_config[self.config.name])

        return info
    
    def _split_generators(self, dl_manager: datasets.DownloadManager) -> List[datasets.SplitGenerator]:
        downloads = dl_manager.download_and_extract(urls_per_split)

        return [
            datasets.SplitGenerator(name=datasets.Split.TRAIN, gen_kwargs={"filepath": downloads["train"]})
        ]
    
    def _generate_examples(self, filepath: str):
        data = pandas.read_csv(filepath)
        data = self.preprocess(data, config=self.config.name)

        for row_id, row in data.iterrows():
            data_row = dict(row)

            yield row_id, data_row


    def preprocess(self, data: pandas.DataFrame, config: str = "higgs") -> pandas.DataFrame:       
        return data[list(features_types_per_config[config].keys())]
