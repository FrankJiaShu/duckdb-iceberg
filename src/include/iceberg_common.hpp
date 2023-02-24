//===----------------------------------------------------------------------===//
//                         DuckDB
//
// iceberg_common.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/common/printer.hpp"
#include "yyjson.hpp"

namespace duckdb {

struct IceBergSnapshot {
	uint64_t snapshot_id;
	uint64_t sequence_number;
	uint64_t schema_id;
	string manifest_list;
	timestamp_t timestamp_ms;
};

static uint64_t TryGetNumFromObject(yyjson_val *obj, string field) {
	auto val = yyjson_obj_getn(obj, field.c_str(), field.size());
	if (!val || yyjson_get_tag(val) != YYJSON_TYPE_NUM) {
		throw IOException("Invalid field found while parsing field: " + field);
	}
	return yyjson_get_uint(val);
}

static string TryGetStrFromObject(yyjson_val *obj, string field) {
	auto val = yyjson_obj_getn(obj, field.c_str(), field.size());
	if (!val || yyjson_get_tag(val) != YYJSON_TYPE_STR) {
		throw IOException("Invalid field found while parsing field: " + field);
	}
	return yyjson_get_str(val);
}

static IceBergSnapshot ParseSnapShot(yyjson_val *snapshot) {
	IceBergSnapshot ret;

	auto snapshot_tag = yyjson_get_tag(snapshot);
	if (snapshot_tag != YYJSON_TYPE_OBJ) {
		throw IOException("Invalid snapshot field found parsing iceberg metadata.json");
	}

	ret.snapshot_id = TryGetNumFromObject(snapshot, "snapshot-id");
	ret.sequence_number = TryGetNumFromObject(snapshot, "sequence-number");
	ret.timestamp_ms = Timestamp::FromEpochMs(TryGetNumFromObject(snapshot, "timestamp-ms"));
	ret.schema_id = TryGetNumFromObject(snapshot, "schema-id");
	ret.manifest_list = TryGetStrFromObject(snapshot, "manifest-list");

	return ret;
}

static string FileToString(const string &path, FileSystem &fs) {
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	auto file_size = handle->GetFileSize();
	string ret_val(file_size, ' ');
	handle->Read((char *)ret_val.c_str(), file_size);
	return ret_val;
}

static idx_t GetTableVersion(string &path, FileSystem &fs) {
	auto meta_path = fs.JoinPath(path, "metadata");
	auto version_file_path = FileSystem::JoinPath(meta_path, "version-hint.text");
	auto version_file_content = FileToString(version_file_path, fs);

	try {
		return std::stoll(version_file_content);
	} catch (std::invalid_argument &e) {
		throw IOException("Iceberg version hint file contains invalid value");
	} catch (std::out_of_range &e) {
		throw IOException("Iceberg version hint file contains invalid value");
	}
}

static yyjson_val *FindLatestSnapshotInternal(yyjson_val *snapshots) {
	size_t idx, max;
	yyjson_val *snapshot;

	uint64_t max_timestamp = NumericLimits<uint64_t>::Minimum();
	yyjson_val *max_snapshot = nullptr;

	yyjson_arr_foreach(snapshots, idx, max, snapshot) {

		auto timestamp = TryGetNumFromObject(snapshot, "timestamp-ms");
		if (timestamp >= max_timestamp) {
			max_timestamp = timestamp;
			max_snapshot = snapshot;
		}
	}

	return max_snapshot;
}

static yyjson_val *FindSnapshotByIdInternal(yyjson_val *snapshots, idx_t target_id) {
	size_t idx, max;
	yyjson_val *snapshot;

	yyjson_arr_foreach(snapshots, idx, max, snapshot) {

		auto snapshot_id = TryGetNumFromObject(snapshot, "snapshot-id");

		if (snapshot_id == target_id) {
			return snapshot;
		}
	}

	return nullptr;
}

static yyjson_val *FindSnapshotByIdTimestampInternal(yyjson_val *snapshots, timestamp_t timestamp) {
	size_t idx, max;
	yyjson_val *snapshot;

	uint64_t max_millis = NumericLimits<uint64_t>::Minimum();
	yyjson_val *max_snapshot = nullptr;

	auto timestamp_millis = Timestamp::GetEpochMs(timestamp);

	yyjson_arr_foreach(snapshots, idx, max, snapshot) {
		auto curr_millis = TryGetNumFromObject(snapshot, "timestamp-ms");

		if (curr_millis <= timestamp_millis && curr_millis >= max_millis) {
			max_snapshot = snapshot;
			max_millis = curr_millis;
		}
	}

	return max_snapshot;
}

static string ReadMetaData(string &path, FileSystem &fs) {
	auto table_version = GetTableVersion(path, fs);

	auto meta_path = fs.JoinPath(path, "metadata");
	auto metadata_file_path = fs.JoinPath(meta_path, "v" + to_string(table_version) + ".metadata.json");

	return FileToString(metadata_file_path, fs);
}

static IceBergSnapshot GetLatestSnapshot(string &path, FileSystem &fs) {
	auto metadata_json = ReadMetaData(path, fs);
	auto doc = yyjson_read(metadata_json.c_str(), metadata_json.size(), 0);
	auto root = yyjson_doc_get_root(doc);
	auto snapshots = yyjson_obj_get(root, "snapshots");
	auto latest_snapshot = FindLatestSnapshotInternal(snapshots);

	if (!latest_snapshot) {
		throw IOException("No snapshots found");
	}

	return ParseSnapShot(latest_snapshot);
}

static IceBergSnapshot GetSnapshotById(string &path, FileSystem &fs, idx_t snapshot_id) {
	auto metadata_json = ReadMetaData(path, fs);
	auto doc = yyjson_read(metadata_json.c_str(), metadata_json.size(), 0);
	auto root = yyjson_doc_get_root(doc);
	auto snapshots = yyjson_obj_get(root, "snapshots");
	auto snapshot = FindSnapshotByIdInternal(snapshots, snapshot_id);

	if (!snapshot) {
		throw IOException("Could not find snapshot with id " + to_string(snapshot_id));
	}

	return ParseSnapShot(snapshot);
}

static IceBergSnapshot GetSnapshotByTimestamp(string &path, FileSystem &fs, timestamp_t timestamp) {
	auto metadata_json = ReadMetaData(path, fs);
	auto doc = yyjson_read(metadata_json.c_str(), metadata_json.size(), 0);
	auto root = yyjson_doc_get_root(doc);
	auto snapshots = yyjson_obj_get(root, "snapshots");
	auto snapshot = FindSnapshotByIdTimestampInternal(snapshots, timestamp);

	if (!snapshot) {
		throw IOException("Could not find latest snapshots for timestamp " + Timestamp::ToString(timestamp));
	}

	return ParseSnapShot(snapshot);
}

} // namespace duckdb