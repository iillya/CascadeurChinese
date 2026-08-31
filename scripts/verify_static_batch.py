"""Verify reviewed strings against host binaries and preserve batch provenance."""
import gzip
import argparse
import json
from pathlib import Path
from scan_ui_inventory import ASCII, UTF16, pe_regions
from merge_reviewed_ui import read

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--batch', type=Path, default=ROOT / 'analysis/static_ui_reviewed_zh.json')
    parser.add_argument('--sources', type=Path, default=ROOT / 'analysis/static_ui_reviewed_sources.json')
    args = parser.parse_args()
    batch = read(args.batch)['translations']
    formal = read(ROOT / 'translations/dictionary_zh.json')['translations']
    evidence = {key: [] for key in batch}
    for path in Path('C:/Program Files/Cascadeur').iterdir():
        if path.suffix.lower() not in {'.dll', '.exe'}:
            continue
        data = path.read_bytes()
        for start, size, section in pe_regions(data):
            block = data[start:start+size]
            for pattern, encoding in ((ASCII, 'ascii'), (UTF16, 'utf-16-le')):
                for match in pattern.finditer(block):
                    key = match[0].decode(encoding).strip()
                    if key in evidence:
                        evidence[key].append({'file': path.name, 'section': section,
                                              'file_offset': start+match.start(), 'encoding': encoding})
    missing = [key for key, locations in evidence.items() if not locations]
    assert not missing, missing
    assert all(formal[key] == value for key, value in batch.items())
    output = args.sources
    output.write_text(json.dumps(evidence, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    scan = ROOT / 'analysis/static-ui-20260831-162207'
    with gzip.open(scan / 'all_terms.json.gz', 'rb') as stream:
        while stream.read(1024*1024):
            pass  # Read through EOF to verify gzip CRC without loading the pool.
    review = read(scan / 'ui_review_zh.json')
    review['translations'] = {k:v for k,v in review['translations'].items() if k not in formal}
    (scan / 'remaining_review_zh.json').write_text(json.dumps(review, ensure_ascii=True, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({'verified_batch': len(batch), 'formal_total': len(formal),
                      'remaining_shortlist': len(review['translations']), 'gzip_crc': 'passed'}))


if __name__ == '__main__':
    main()
