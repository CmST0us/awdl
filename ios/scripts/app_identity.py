"""Installed app identities from the versioned Xcode configurations."""
from pathlib import Path

CONFIGURATIONS = Path(__file__).resolve().parents[1] / 'Configurations'


def setting(configuration, key):
    for line in (CONFIGURATIONS / configuration).read_text().splitlines():
        name, separator, value = line.partition('=')
        if separator and name.strip() == key:
            return value.strip()
    raise ValueError(f'{configuration} has no {key}')


APP_BUNDLE = setting('AwareLab.xcconfig', 'PRODUCT_BUNDLE_IDENTIFIER')
TEST_BUNDLE = setting('AwareLabUITests.xcconfig', 'PRODUCT_BUNDLE_IDENTIFIER')
RUNNER_BUNDLE = TEST_BUNDLE + '.xctrunner'
