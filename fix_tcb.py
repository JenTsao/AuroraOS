import os
import re

fixes = [
    (r'->task\.task', '->task'),
    (r'->scheduler\.scheduler', '->scheduler'),
    (r'->memory\.memory', '->memory'),
    (r'->ipc\.ipc', '->ipc'),
    (r'->security\.security', '->security'),
    (r'\.task\.task', '.task'),
    (r'\.scheduler\.scheduler', '.scheduler'),
    (r'\.memory\.memory', '.memory'),
    (r'\.ipc\.ipc', '.ipc'),
    (r'\.security\.security', '.security')
]

def fix_file(filepath):
    with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
        content = f.read()
    
    orig = content
    for old, new in fixes:
        content = re.sub(old, new, content)
        
    if orig != content:
        with open(filepath, 'w', encoding='utf-8') as f:
            f.write(content)
        print(f"Fixed {filepath}")

def main():
    search_dirs = ['kernel', 'tests', 'boot', 'apps', 'net', 'ui', 'vfs', 'metrics', 'experimental', 'syscall']
    for d in search_dirs:
        for root, _, files in os.walk(d):
            for file in files:
                if file.endswith('.cpp') or file.endswith('.hpp') or file.endswith('.c') or file.endswith('.h'):
                    fix_file(os.path.join(root, file))

if __name__ == '__main__':
    main()
