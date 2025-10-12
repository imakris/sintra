#!/usr/bin/env python3
"""Remove unused Boost headers from the vendored directory."""
import os
import sys

def main():
    # Read the list of unused headers
    with open('boost_unused_headers.md', 'r') as f:
        unused = [line.strip().replace('\\', '/') for line in f if line.strip()]

    boost_root = 'third_party/boost/boost'

    if not os.path.exists(boost_root):
        print(f"Error: Boost directory not found at {boost_root}")
        return 1

    # Calculate sizes before deletion
    total_files = 0
    total_size = 0
    for root, dirs, files in os.walk(boost_root):
        for name in files:
            path = os.path.join(root, name)
            total_files += 1
            total_size += os.path.getsize(path)

    print(f"Before pruning:")
    print(f"  Total files: {total_files}")
    print(f"  Total size: {total_size / (1024*1024):.1f} MB")
    print(f"  Files to remove: {len(unused)}")
    print()

    # Ask for confirmation
    response = input("Proceed with deletion? (yes/no): ")
    if response.lower() not in ('yes', 'y'):
        print("Aborted.")
        return 0

    # Delete unused files
    deleted_count = 0
    deleted_size = 0
    not_found = 0

    for header in unused:
        path = os.path.join(boost_root, header.replace('/', os.sep))
        if os.path.exists(path):
            size = os.path.getsize(path)
            try:
                os.remove(path)
                deleted_count += 1
                deleted_size += size
                if deleted_count % 100 == 0:
                    print(f"Deleted {deleted_count} files...")
            except OSError as e:
                print(f"Warning: Could not delete {path}: {e}")
        else:
            not_found += 1

    print()
    print(f"Deletion complete:")
    print(f"  Files deleted: {deleted_count}")
    print(f"  Files not found: {not_found}")
    print(f"  Space freed: {deleted_size / (1024*1024):.1f} MB")
    print()

    # Remove empty directories
    print("Removing empty directories...")
    removed_dirs = 0
    for root, dirs, files in os.walk(boost_root, topdown=False):
        for dir_name in dirs:
            dir_path = os.path.join(root, dir_name)
            try:
                if not os.listdir(dir_path):  # Directory is empty
                    os.rmdir(dir_path)
                    removed_dirs += 1
            except OSError:
                pass  # Directory not empty or other error

    print(f"  Empty directories removed: {removed_dirs}")
    print()

    # Calculate sizes after deletion
    remaining_files = 0
    remaining_size = 0
    for root, dirs, files in os.walk(boost_root):
        for name in files:
            path = os.path.join(root, name)
            remaining_files += 1
            remaining_size += os.path.getsize(path)

    print(f"After pruning:")
    print(f"  Remaining files: {remaining_files}")
    print(f"  Remaining size: {remaining_size / (1024*1024):.1f} MB")
    print(f"  Reduction: {(deleted_size / total_size * 100):.1f}%")

    return 0

if __name__ == '__main__':
    sys.exit(main())
