import os
import shutil
import sys

def copy_headers(src_dir, dest_dir, prefix):
    for root, dirs, files in os.walk(src_dir):
        for file in files:
            if file.endswith('.hpp') or file.endswith('.h'):
                rel_path = os.path.relpath(root, src_dir)
                dest_path = os.path.join(dest_dir, prefix, rel_path)
                os.makedirs(dest_path, exist_ok=True)
                shutil.copy2(os.path.join(root, file), dest_path)

def main():
    root_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
    sdk_dir = os.path.join(root_dir, 'sdk')
    include_dir = os.path.join(sdk_dir, 'include', 'auroraos')
    
    print("Packaging Aurora SDK...")
    
    # 1. Package API Headers
    print("Copying runtime headers...")
    copy_headers(os.path.join(root_dir, 'runtime'), os.path.join(include_dir, 'runtime'), '')
    
    print("Copying syscall headers...")
    copy_headers(os.path.join(root_dir, 'syscall'), os.path.join(include_dir, 'syscall'), '')
    
    print("Copying service APIs (IPC structures)...")
    # In a microkernel, apps interact with services via IPC syscalls and struct definitions
    copy_headers(os.path.join(root_dir, 'services', 'ui'), os.path.join(include_dir, 'services', 'ui'), '')
    copy_headers(os.path.join(root_dir, 'services', 'sensor'), os.path.join(include_dir, 'services', 'sensor'), '')
    
    # 2. Package toolchain (CMake)
    print("Aurora SDK packaged successfully at:", sdk_dir)

if __name__ == '__main__':
    main()
