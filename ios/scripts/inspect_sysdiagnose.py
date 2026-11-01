#!/usr/bin/env python3
"""Validate downloaded bytes and extract only diagnostic text/logarchive, no links."""
import argparse, hashlib, json, pathlib, shutil, subprocess, tarfile
p = argparse.ArgumentParser(); p.add_argument('directory',type=pathlib.Path); a=p.parse_args()
meta=json.loads((a.directory/'download.json').read_text())
archive=a.directory/meta['archive']
assert archive.stat().st_size == meta['expected_length'] == meta['actual_length']
digest=hashlib.sha256()
with archive.open('rb') as source:
    for chunk in iter(lambda:source.read(1024*1024),b''): digest.update(chunk)
assert digest.hexdigest() == meta['sha256']
with (a.directory/'gzip-test.log').open('w') as log:
    result=subprocess.run(['gzip','-t',str(archive)],stdout=log,stderr=subprocess.STDOUT,timeout=180)
assert result.returncode == 0, 'gzip integrity failed'
extracted=a.directory/'inspect'; extracted.mkdir(exist_ok=False)
text_files=[]; logs=set(); total=0
with tarfile.open(archive,'r|gz') as tar, (a.directory/'tar-index.jsonl').open('w') as index:
    for entry in tar:
        index.write(json.dumps({'name':entry.name,'size':entry.size,'type':entry.type.decode(errors='replace')})+'\n')
        total+=1
        path=pathlib.PurePosixPath(entry.name)
        assert not path.is_absolute() and '..' not in path.parts, 'unsafe archive member'
        is_log=any(part.endswith('.logarchive') for part in path.parts)
        is_text=entry.size <= 4*1024*1024 and any(key in path.name.lower() for key in ['sysdiagnose','systemversion','osversion','systeminfo','uname'])
        if entry.isfile() and (is_log or is_text):
            target=extracted/path
            target.parent.mkdir(parents=True,exist_ok=True)
            with tar.extractfile(entry) as src, target.open('wb') as dst: shutil.copyfileobj(src,dst)
            if is_log:
                parts=path.parts; n=next(i for i,x in enumerate(parts) if x.endswith('.logarchive'));logs.add(str(extracted.joinpath(*parts[:n+1])))
            else: text_files.append(str(target.relative_to(a.directory)))
(a.directory/'archive-validation.json').write_text(json.dumps({'status':'INTEGRITY_PASS_CONTENT_REVIEW_REQUIRED','gzip_returncode':result.returncode,'sha256':digest.hexdigest(),'members':total,'text_files':text_files,'logarchives':sorted(logs)},indent=2))
print(json.dumps({'members':total,'text_files':text_files,'logarchives':sorted(logs)},indent=2))
