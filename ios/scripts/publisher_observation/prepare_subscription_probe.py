#!/usr/bin/env python3
"""Freeze a local-only iPad source-PID comparison package; no device access."""
import argparse
import json
from pathlib import Path
import re
import shutil


def main():
    p=argparse.ArgumentParser();p.add_argument('--run-id',required=True)
    inputs=p.add_mutually_exclusive_group(required=True)
    inputs.add_argument('--installed-manifest',type=Path)
    inputs.add_argument('--built-manifest',type=Path)
    p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]+',a.run_id):p.error('invalid run ID')
    input_path=a.installed_manifest or a.built_manifest
    installed=json.loads(input_path.read_text())
    if a.installed_manifest and installed.get('installed') is not True:p.error('installed input evidence required')
    if a.built_manifest and installed.get('installed') is not False:p.error('built inputs must remain uninstalled')
    if a.output.exists():p.error('manifest already exists')
    root=Path(__file__).resolve().parent
    stage=a.output.resolve().parent/('subscription-inputs-'+a.run_id)
    stage.mkdir(exist_ok=False)
    project=root.parents[2]
    sources=[]
    inputs=list(root.glob('*.py'))
    inputs.extend(root.parent/name for name in ('app_identity.py','service_contract.py'))
    inputs.extend(project/'config'/name for name in ('fileserver-discovery.toml','wifiawared.toml'))
    inputs.extend((project/'ios/Configurations').glob('*.xcconfig'))
    for source in inputs:
        destination=stage/source.relative_to(project)
        destination.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(source,destination)
        sources.append(dict(source=str(source),frozen=str(destination)))
    program_root=stage/root.relative_to(project)
    config=dict(run_id=a.run_id,udid=installed['udid'],xctestrun=installed['xctestrun'],
                python=installed['collector_argv'][0],collector=str(program_root/'collector.py'),
                program=str(program_root/'subscription_probe.py'),installed=installed['installed'],
                installation_manifest=str(input_path.resolve()),installation_evidence=installed.get('installation_evidence'),
                signed_artifacts=installed['signed_artifacts'],sources=sources,
                activity_seconds=12,total_seconds=15,readiness_seconds=1,pid_prepared=False,device_execution_authorized=False,
                inherited_admission_failures=installed['inherited_admission_failures'])
    # The reviewed observation manifest names the cached Python runtime explicitly.
    if Path(config['python']).name!='python3':p.error('installed manifest must name the reviewed Python collector runtime')
    a.output.write_text(json.dumps(config,indent=2)+'\n');print(a.output)


if __name__=='__main__':main()
