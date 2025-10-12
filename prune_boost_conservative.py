#!/usr/bin/env python3
"""Conservative Boost pruning - only remove files that actually exist and are confirmed unused."""
import os
import sys

def main():
    # Read the list of used headers (these are the ones we MUST keep)
    with open('boost_used_headers.md', 'r') as f:
        used_headers = set(line.strip().replace('\\', '/') for line in f if line.strip())

    boost_root = 'third_party/boost/boost'

    if not os.path.exists(boost_root):
        print(f"Error: Boost directory not found at {boost_root}")
        return 1

    # Find all actual files on disk
    actual_files = []
    for root, dirs, files in os.walk(boost_root):
        for name in files:
            path = os.path.join(root, name)
            rel_path = os.path.relpath(path, boost_root).replace('\\', '/')
            actual_files.append((path, rel_path))

    print(f"Analysis:")
    print(f"  Used headers (from analysis): {len(used_headers)}")
    print(f"  Actual files on disk: {len(actual_files)}")
    print()

    # Identify files to keep and remove
    # Some Boost components include headers indirectly via macros that resolve
    # to compiler-specific preprocessed paths (e.g. mpl/aux_/preprocessed/**).
    # Ensure those directories are always kept to avoid accidental breakage.
    always_keep_prefixes = (
        'mpl/aux_/preprocessed/',
    )
    to_keep = []
    to_remove = []
    total_size = 0
    remove_size = 0

    for path, rel_path in actual_files:
        size = os.path.getsize(path)
        total_size += size

        if rel_path in used_headers or any(rel_path.startswith(p) for p in always_keep_prefixes):
            to_keep.append((path, rel_path, size))
        else:
            to_remove.append((path, rel_path, size))
            remove_size += size

    print(f"Files to keep: {len(to_keep)} ({(len(to_keep)/len(actual_files)*100):.1f}%)")
    print(f"Files to remove: {len(to_remove)} ({(len(to_remove)/len(actual_files)*100):.1f}%)")
    print(f"Space to free: {remove_size / (1024*1024):.1f} MB out of {total_size / (1024*1024):.1f} MB")
    print()

    # Show some examples of what will be removed
    print("Sample files to be removed:")
    for i, (path, rel_path, size) in enumerate(to_remove[:10]):
        print(f"  {rel_path}")
    if len(to_remove) > 10:
        print(f"  ... and {len(to_remove) - 10} more")
    print()

    # Ask for confirmation
    response = input("Proceed with deletion? (yes/no): ")
    if response.lower() not in ('yes', 'y'):
        print("Aborted.")
        return 0

    # Delete unused files
    deleted_count = 0
    deleted_size = 0

    for path, rel_path, size in to_remove:
        try:
            os.remove(path)
            deleted_count += 1
            deleted_size += size
            if deleted_count % 100 == 0:
                print(f"Deleted {deleted_count}/{len(to_remove)} files...")
        except OSError as e:
            print(f"Warning: Could not delete {path}: {e}")

    print()
    print(f"Deletion complete:")
    print(f"  Files deleted: {deleted_count}")
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

    # Verify remaining files
    remaining_files = []
    remaining_size = 0
    for root, dirs, files in os.walk(boost_root):
        for name in files:
            path = os.path.join(root, name)
            remaining_files.append(path)
            remaining_size += os.path.getsize(path)

    print(f"After pruning:")
    print(f"  Remaining files: {len(remaining_files)}")
    print(f"  Remaining size: {remaining_size / (1024*1024):.1f} MB")
    print(f"  Reduction: {(deleted_size / total_size * 100):.1f}%")

    return 0

if __name__ == '__main__':
    sys.exit(main())
