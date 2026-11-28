module;

#include <faio/fs/directory.hpp>
#include <faio/fs/file.hpp>
#include <faio/fs/path.hpp>

export module faio.fs;
export import faio.io;

export namespace faio::fs {
using ::faio::fs::dir_builder;
using ::faio::fs::dir_entry;
using ::faio::fs::file;
using ::faio::fs::file_metadata;
using ::faio::fs::open_options;
using ::faio::fs::permissions;
using ::faio::fs::directory_reader;
using ::faio::fs::canonicalize;
using ::faio::fs::copy;
using ::faio::fs::create_dir;
using ::faio::fs::create_dir_all;
using ::faio::fs::hard_link;
using ::faio::fs::metadata;
using ::faio::fs::path;
using ::faio::fs::read;
using ::faio::fs::read_dir;
using ::faio::fs::read_to_string;
using ::faio::fs::recursive_remove_options;
using ::faio::fs::remove_dir;
using ::faio::fs::remove_dir_all;
using ::faio::fs::remove_file;
using ::faio::fs::rename;
using ::faio::fs::seek_from;
using ::faio::fs::set_permissions;
using ::faio::fs::symlink;
using ::faio::fs::symlink_metadata;
using ::faio::fs::write;
}  // namespace faio::fs
