#include <Storages/StorageKeeperMap.h>

#include <Columns/ColumnString.h>

#include <Databases/DatabaseReplicated.h>

#include <Core/NamesAndTypes.h>
#include <Core/UUID.h>
#include <Core/ServerUUID.h>

#include <DataTypes/DataTypeString.h>

#include <Interpreters/evaluateConstantExpression.h>

#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTSelectQuery.h>

#include <Processors/ISource.h>
#include <Processors/Sinks/SinkToStorage.h>

#include <Storages/ColumnsDescription.h>
#include <Storages/KVStorageUtils.h>
#include <Storages/StorageFactory.h>
#include <Storages/StorageInMemoryMetadata.h>
#include <Storages/checkAndGetLiteralArgument.h>

#include <Common/Base64.h>
#include <Common/Exception.h>
#include <Common/ZooKeeper/IKeeper.h>
#include <Common/ZooKeeper/KeeperException.h>
#include <Common/ZooKeeper/Types.h>
#include <Common/ZooKeeper/ZooKeeper.h>
#include <Common/ZooKeeper/ZooKeeperConstants.h>

#include <base/types.h>

#include <boost/algorithm/string/classification.hpp>

namespace DB
{

namespace ErrorCodes
{
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int BAD_ARGUMENTS;
    extern const int KEEPER_EXCEPTION;
    extern const int LOGICAL_ERROR;
    extern const int LIMIT_EXCEEDED;
}

namespace
{

std::string_view getBaseName(const std::string_view path)
{
    auto last_slash = path.find_last_of('/');
    if (last_slash == std::string_view::npos)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Failed to get basename of path '{}'", path);

    return path.substr(last_slash + 1);
}

}

class StorageKeeperMapSink : public SinkToStorage
{
    StorageKeeperMap & storage;
    std::unordered_map<std::string, std::string> new_values;
    size_t primary_key_pos;

public:
    StorageKeeperMapSink(StorageKeeperMap & storage_, const StorageMetadataPtr & metadata_snapshot)
        : SinkToStorage(metadata_snapshot->getSampleBlock()), storage(storage_)
    {
        auto primary_key = storage.getPrimaryKey();
        assert(primary_key.size() == 1);
        primary_key_pos = getHeader().getPositionByName(primary_key[0]);
    }

    std::string getName() const override { return "StorageKeeperMapSink"; }

    void consume(Chunk chunk) override
    {
        auto rows = chunk.getNumRows();
        auto block = getHeader().cloneWithColumns(chunk.detachColumns());

        WriteBufferFromOwnString wb_key;
        WriteBufferFromOwnString wb_value;

        for (size_t i = 0; i < rows; ++i)
        {
            wb_key.restart();
            wb_value.restart();

            size_t idx = 0;
            for (const auto & elem : block)
            {
                elem.type->getDefaultSerialization()->serializeBinary(*elem.column, i, idx == primary_key_pos ? wb_key : wb_value);
                ++idx;
            }

            auto key = base64Encode(wb_key.str(), true);
            new_values[std::move(key)] = std::move(wb_value.str());
        }
    }

    void onFinish() override
    {
        auto zookeeper = storage.getClient();

        Coordination::Requests requests;

        auto keys_limit = storage.keysLimit();

        size_t current_keys_num = 0;
        size_t new_keys_num = 0;

        if (keys_limit != 0)
        {
            Coordination::Stat root_stat;
            zookeeper->get(storage.rootKeeperPath(), &root_stat);
            // exclude metadata node
            current_keys_num = root_stat.numChildren - 1;
        }

        std::vector<std::pair<const std::string *, std::future<Coordination::ExistsResponse>>> exist_responses;
        for (const auto & [key, value] : new_values)
        {
            auto path = storage.fullPathForKey(key);

            exist_responses.push_back({&key, zookeeper->asyncExists(path)});
        }

        for (auto & [key, response] : exist_responses)
        {
            if (response.get().error == Coordination::Error::ZOK)
            {
                requests.push_back(zkutil::makeSetRequest(storage.fullPathForKey(*key), new_values[*key], -1));
            }
            else
            {
                requests.push_back(zkutil::makeCreateRequest(storage.fullPathForKey(*key), new_values[*key], zkutil::CreateMode::Persistent));
                ++new_keys_num;
            }
        }

        if (new_keys_num != 0)
        {
            auto will_be = current_keys_num + new_keys_num;
            if (keys_limit != 0 && will_be > keys_limit)
                throw Exception(
                    ErrorCodes::LIMIT_EXCEEDED,
                    "Limit would be exceeded by inserting {} new key(s). Limit is {}, while the number of keys would be {}",
                    new_keys_num,
                    keys_limit,
                    will_be);
        }

        zookeeper->multi(requests);
    }
};

template <typename KeyContainer>
class StorageKeeperMapSource : public ISource
{
    const StorageKeeperMap & storage;
    size_t max_block_size;

    using KeyContainerPtr = std::shared_ptr<KeyContainer>;
    KeyContainerPtr container;
    using KeyContainerIter = typename KeyContainer::const_iterator;
    KeyContainerIter it;
    KeyContainerIter end;

public:
    StorageKeeperMapSource(
        const StorageKeeperMap & storage_,
        const Block & header,
        size_t max_block_size_,
        KeyContainerPtr container_,
        KeyContainerIter begin_,
        KeyContainerIter end_)
        : ISource(header), storage(storage_), max_block_size(max_block_size_), container(std::move(container_)), it(begin_), end(end_)
    {
    }

    std::string getName() const override { return "StorageKeeperMapSource"; }

    Chunk generate() override
    {
        if (it >= end)
        {
            it = {};
            return {};
        }

        using KeyType = typename KeyContainer::value_type;
        if constexpr (std::same_as<KeyType, Field>)
        {
            const auto & sample_block = getPort().getHeader();
            const auto & key_column_type = sample_block.getByName(storage.getPrimaryKey().at(0)).type;
            auto raw_keys = serializeKeysToRawString(it, end, key_column_type, max_block_size);

            for (auto & raw_key : raw_keys)
                raw_key = base64Encode(raw_key, true);

            return storage.getBySerializedKeys(raw_keys, nullptr);
        }
        else
        {
            size_t elem_num = std::min(max_block_size, static_cast<size_t>(end - it));
            auto chunk = storage.getBySerializedKeys(std::span{it, it + elem_num}, nullptr);
            it += elem_num;
            return chunk;
        }
    }
};

StorageKeeperMap::StorageKeeperMap(
    ContextPtr context_,
    const StorageID & table_id,
    const StorageInMemoryMetadata & metadata,
    bool attach,
    std::string_view primary_key_,
    const std::string & root_path_,
    bool create_missing_root_path,
    UInt64 keys_limit_)
    : IStorage(table_id)
    , WithContext(context_->getGlobalContext())
    , root_path(zkutil::extractZooKeeperPath(root_path_, false))
    , primary_key(primary_key_)
    , zookeeper_name(zkutil::extractZooKeeperName(root_path_))
    , keys_limit(keys_limit_)
    , log(&Poco::Logger::get("StorageKeeperMap"))
{
    std::string path_prefix = context_->getConfigRef().getString("keeper_map_path_prefix", "");
    if (path_prefix.empty())
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "KeeperMap is disabled because 'keeper_map_path_prefix' config is not defined");

    auto database = DatabaseCatalog::instance().getDatabase(table_id.database_name);
    if (!table_id.hasUUID())
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "KeeperMap cannot be used with '{}' database because it uses {} engine. Please use Atomic or Replicated database", table_id.getDatabaseName(), database->getEngineName());

    setInMemoryMetadata(metadata);

    if (root_path.empty())
        throw Exception("root_path should not be empty", ErrorCodes::BAD_ARGUMENTS);
    if (!root_path.starts_with('/'))
        throw Exception("root_path should start with '/'", ErrorCodes::BAD_ARGUMENTS);

    auto config_keys_limit = context_->getConfigRef().getUInt64("keeper_map_keys_limit", 0);
    if (config_keys_limit != 0 && keys_limit > config_keys_limit)
    {
        LOG_WARNING(
            log,
            "Keys limit for {} defined by argument ({}) is larger than the one defined by 'keeper_map_keys_limit' config ({}). Will use "
            "config defined value",
            getStorageID().getFullTableName(),
            keys_limit,
            config_keys_limit);
        keys_limit = config_keys_limit;
    }
    else if (keys_limit > 0)
    {
        LOG_INFO(log, "Keys limit for {} will be set to {}", getStorageID().getFullTableName(), keys_limit);
    }

    auto root_path_fs = fs::path(path_prefix) / std::string_view{root_path}.substr(1);
    root_path = root_path_fs.generic_string();

    auto metadata_path_fs = root_path_fs / "ch_metadata";
    metadata_path = metadata_path_fs;
    tables_path = metadata_path_fs / "tables";

    auto table_unique_id = toString(table_id.uuid) + toString(ServerUUID::get());
    table_path = fs::path(tables_path) / table_unique_id;

    dropped_path = metadata_path_fs / "dropped";
    dropped_lock_path = fs::path(dropped_path) / "lock";

    if (attach)
    {
        checkTable<false>();
        return;
    }

    auto client = getClient();

    if (root_path != "/" && !client->exists(root_path))
    {
        if (!create_missing_root_path)
        {
            throw Exception(
                ErrorCodes::BAD_ARGUMENTS,
                "Path '{}' doesn't exist. Please create it or set 'create_missing_root_path' to true'",
                root_path_);
        }
        else
        {
            LOG_TRACE(log, "Creating root path {}", root_path);
            client->createAncestors(root_path);
            client->createIfNotExists(root_path, "");
        }
    }

    for (size_t i = 0; i < 1000; ++i)
    {
        if (client->exists(dropped_path))
        {
            LOG_INFO(log, "Removing leftover nodes");
            auto code = client->tryCreate(dropped_lock_path, "", zkutil::CreateMode::Ephemeral);

            if (code == Coordination::Error::ZNONODE ||  code == Coordination::Error::ZNODEEXISTS)
            {
                LOG_INFO(log, "Someone else removed leftover nodes");
            }
            else if (code != Coordination::Error::ZOK)
            {
                throw Coordination::Exception(code, dropped_lock_path);
            }
            else
            {
                auto metadata_drop_lock = zkutil::EphemeralNodeHolder::existing(dropped_lock_path, *client);
                if (!dropTable(client, metadata_drop_lock))
                    continue;
            }
        }

        client->createIfNotExists(metadata_path, "");
        client->createIfNotExists(tables_path, "");

        auto code = client->tryCreate(table_path, "", zkutil::CreateMode::Persistent);

        if (code == Coordination::Error::ZOK)
        {
            table_is_valid = true;
            return;
        }

        if (code == Coordination::Error::ZNONODE)
            LOG_INFO(log, "Metadata nodes were deleted in background, will retry");
        else
            throw Coordination::Exception(code, table_path);
    }

    throw Exception(ErrorCodes::BAD_ARGUMENTS, "Cannot create metadata for table, because it is removed concurrently or because of wrong root_path ({})", root_path);
}


Pipe StorageKeeperMap::read(
    const Names & column_names,
    const StorageSnapshotPtr & storage_snapshot,
    SelectQueryInfo & query_info,
    ContextPtr context_,
    QueryProcessingStage::Enum /*processed_stage*/,
    size_t max_block_size,
    unsigned num_streams)
{
    checkTable<true>();
    storage_snapshot->check(column_names);

    FieldVectorPtr filtered_keys;
    bool all_scan;

    Block sample_block = storage_snapshot->metadata->getSampleBlock();
    auto primary_key_type = sample_block.getByName(primary_key).type;
    std::tie(filtered_keys, all_scan) = getFilterKeys(primary_key, primary_key_type, query_info, context_);

    const auto process_keys = [&]<typename KeyContainerPtr>(KeyContainerPtr keys) -> Pipe
    {
        if (keys->empty())
            return {};

        ::sort(keys->begin(), keys->end());
        keys->erase(std::unique(keys->begin(), keys->end()), keys->end());

        Pipes pipes;

        size_t num_keys = keys->size();
        size_t num_threads = std::min<size_t>(num_streams, keys->size());

        assert(num_keys <= std::numeric_limits<uint32_t>::max());
        assert(num_threads <= std::numeric_limits<uint32_t>::max());

        for (size_t thread_idx = 0; thread_idx < num_threads; ++thread_idx)
        {
            size_t begin = num_keys * thread_idx / num_threads;
            size_t end = num_keys * (thread_idx + 1) / num_threads;

            using KeyContainer = typename KeyContainerPtr::element_type;
            pipes.emplace_back(std::make_shared<StorageKeeperMapSource<KeyContainer>>(
                *this, sample_block, max_block_size, keys, keys->begin() + begin, keys->begin() + end));
        }
        return Pipe::unitePipes(std::move(pipes));
    };

    auto client = getClient();
    if (all_scan)
        return process_keys(std::make_shared<std::vector<std::string>>(client->getChildren(root_path)));

    return process_keys(std::move(filtered_keys));
}

SinkToStoragePtr StorageKeeperMap::write(const ASTPtr & /*query*/, const StorageMetadataPtr & metadata_snapshot, ContextPtr /*context*/)
{
    checkTable<true>();
    return std::make_shared<StorageKeeperMapSink>(*this, metadata_snapshot);
}

void StorageKeeperMap::truncate(const ASTPtr &, const StorageMetadataPtr &, ContextPtr, TableExclusiveLockHolder &)
{
    checkTable<true>();
    auto client = getClient();
    client->tryRemoveChildrenRecursive(root_path, true, zkutil::RemoveException{getBaseName(metadata_path), /*remove_subtree_*/ false});
}

bool StorageKeeperMap::dropTable(zkutil::ZooKeeperPtr zookeeper, const zkutil::EphemeralNodeHolder::Ptr & metadata_drop_lock)
{
    zookeeper->removeChildrenRecursive(root_path, zkutil::RemoveException{getBaseName(metadata_path), /*remove_subtree_*/ false});

    bool completely_removed = false;
    Coordination::Requests ops;
    ops.emplace_back(zkutil::makeRemoveRequest(metadata_drop_lock->getPath(), -1));
    ops.emplace_back(zkutil::makeRemoveRequest(dropped_path, -1));
    ops.emplace_back(zkutil::makeRemoveRequest(metadata_path, -1));

    Coordination::Responses responses;
    auto code = zookeeper->tryMulti(ops, responses);
    using enum Coordination::Error;
    switch (code)
    {
        case ZOK:
        {
            metadata_drop_lock->setAlreadyRemoved();
            completely_removed = true;
            LOG_INFO(log, "Metadata in {} was successfully removed from ZooKeeper", metadata_path);
            break;
        }
        case ZNONODE:
            throw Exception(ErrorCodes::LOGICAL_ERROR, "There is a race condition between creation and removal of metadata. It's a bug");
        case ZNOTEMPTY:
            LOG_ERROR(log, "Metadata was not completely removed from ZooKeeper");
            break;
        default:
            zkutil::KeeperMultiException::check(code, ops, responses);
            break;
    }
    return completely_removed;
}

void StorageKeeperMap::drop()
{
    checkTable<true>();
    auto client = getClient();

    client->remove(table_path);

    if (!client->getChildren(tables_path).empty())
        return;

    Coordination::Requests ops;
    Coordination::Responses responses;

    ops.emplace_back(zkutil::makeRemoveRequest(tables_path, -1));
    ops.emplace_back(zkutil::makeCreateRequest(dropped_path, "", zkutil::CreateMode::Persistent));
    ops.emplace_back(zkutil::makeCreateRequest(dropped_lock_path, "", zkutil::CreateMode::Ephemeral));

    auto code = client->tryMulti(ops, responses);

    if (code == Coordination::Error::ZNONODE || code == Coordination::Error::ZNODEEXISTS)
    {
        LOG_INFO(log, "Metadata is being removed by another table");
        return;
    }
    else if (code == Coordination::Error::ZNOTEMPTY)
    {
        LOG_WARNING(log, "Another table is using the same path, metadata will not be deleted");
        return;
    }
    else if (code != Coordination::Error::ZOK)
        zkutil::KeeperMultiException::check(code, ops, responses);

    auto metadata_drop_lock = zkutil::EphemeralNodeHolder::existing(dropped_lock_path, *client);
    dropTable(client, metadata_drop_lock);
}

zkutil::ZooKeeperPtr StorageKeeperMap::getClient() const
{
    std::lock_guard lock{zookeeper_mutex};
    if (!zookeeper_client || zookeeper_client->expired())
    {
        zookeeper_client = nullptr;
        if (zookeeper_name == "default")
            zookeeper_client = getContext()->getZooKeeper();
        else
            zookeeper_client = getContext()->getAuxiliaryZooKeeper(zookeeper_name);

        zookeeper_client->sync(rootKeeperPath());
    }

    return zookeeper_client;
}

const std::string & StorageKeeperMap::rootKeeperPath() const
{
    return root_path;
}

std::string StorageKeeperMap::fullPathForKey(const std::string_view key) const
{
    return fmt::format("{}/{}", root_path, key);
}

UInt64 StorageKeeperMap::keysLimit() const
{
    return keys_limit;
}

std::optional<bool> StorageKeeperMap::isTableValid() const
{
    std::lock_guard lock{init_mutex};
    if (table_is_valid.has_value())
        return *table_is_valid;

    try
    {
        // validate all metadata nodes are present
        Coordination::Requests requests;
        requests.push_back(zkutil::makeCheckRequest(table_path, -1));

        Coordination::Responses responses;
        auto client = getClient();
        auto res = client->tryMulti(requests, responses);
        table_is_valid = res == Coordination::Error::ZOK;
    }
    catch (const Coordination::Exception & e)
    {
        tryLogCurrentException(log);

        if (!Coordination::isHardwareError(e.code))
            table_is_valid = false;
    }
    catch (const Exception &)
    {
        tryLogCurrentException(log);
        table_is_valid = false;
    }

    return table_is_valid;
}

Chunk StorageKeeperMap::getByKeys(const ColumnsWithTypeAndName & keys, PaddedPODArray<UInt8> & null_map, const Names &) const
{
    if (keys.size() != 1)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "StorageKeeperMap supports only one key, got: {}", keys.size());

    auto raw_keys = serializeKeysToRawString(keys[0]);

    if (raw_keys.size() != keys[0].column->size())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Assertion failed: {} != {}", raw_keys.size(), keys[0].column->size());

    return getBySerializedKeys(raw_keys, &null_map);
}

Chunk StorageKeeperMap::getBySerializedKeys(const std::span<const std::string> keys, PaddedPODArray<UInt8> * null_map) const
{
    Block sample_block = getInMemoryMetadataPtr()->getSampleBlock();
    MutableColumns columns = sample_block.cloneEmptyColumns();
    size_t primary_key_pos = getPrimaryKeyPos(sample_block, getPrimaryKey());

    if (null_map)
    {
        null_map->clear();
        null_map->resize_fill(keys.size(), 1);
    }

    auto client = getClient();

    std::vector<std::future<Coordination::GetResponse>> values;
    values.reserve(keys.size());

    for (const auto & key : keys)
    {
        const auto full_path = fullPathForKey(key);
        if (full_path == metadata_path)
        {
            values.emplace_back();
            continue;
        }

        values.emplace_back(client->asyncTryGet(full_path));
    }

    auto wait_until = std::chrono::system_clock::now() + std::chrono::milliseconds(Coordination::DEFAULT_OPERATION_TIMEOUT_MS);

    for (size_t i = 0; i < keys.size(); ++i)
    {
        auto & value = values[i];
        if (!value.valid())
            continue;

        if (value.wait_until(wait_until) != std::future_status::ready)
            throw DB::Exception(ErrorCodes::LOGICAL_ERROR, "Failed to fetch values: timeout");

        auto response = value.get();
        Coordination::Error code = response.error;

        if (code == Coordination::Error::ZOK)
        {
            fillColumns(base64Decode(keys[i], true), response.data, primary_key_pos, sample_block, columns);
        }
        else if (code == Coordination::Error::ZNONODE)
        {
            if (null_map)
            {
                (*null_map)[i] = 0;
                for (size_t col_idx = 0; col_idx < sample_block.columns(); ++col_idx)
                    columns[col_idx]->insert(sample_block.getByPosition(col_idx).type->getDefault());
            }
        }
        else
        {
            throw DB::Exception(ErrorCodes::KEEPER_EXCEPTION, "Failed to fetch value: {}", code);
        }
    }

    size_t num_rows = columns.at(0)->size();
    return Chunk(std::move(columns), num_rows);
}

Block StorageKeeperMap::getSampleBlock(const Names &) const
{
    auto metadata = getInMemoryMetadataPtr();
    return metadata ? metadata->getSampleBlock() : Block();
}

namespace
{

StoragePtr create(const StorageFactory::Arguments & args)
{
    ASTs & engine_args = args.engine_args;
    if (engine_args.empty() || engine_args.size() > 3)
        throw Exception(
            ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
            "Storage KeeperMap requires 1-3 arguments:\n"
            "root_path: path in the Keeper where the values will be stored (required)\n"
            "create_missing_root_path: 1 if the root path should be created if it's missing, otherwise throw exception (default: 1)\n",
            "keys_limit: number of keys allowed to be stored, 0 is no limit (default: 0)");

    const auto root_path_node = evaluateConstantExpressionAsLiteral(engine_args[0], args.getLocalContext());
    auto root_path = checkAndGetLiteralArgument<std::string>(root_path_node, "root_path");

    bool create_missing_root_path = true;
    if (engine_args.size() > 1)
        create_missing_root_path = checkAndGetLiteralArgument<UInt64>(engine_args[1], "create_missing_root_path");

    UInt64 keys_limit = 0;
    if (engine_args.size() > 2)
        keys_limit = checkAndGetLiteralArgument<UInt64>(engine_args[2], "keys_limit");

    StorageInMemoryMetadata metadata;
    metadata.setColumns(args.columns);
    metadata.setConstraints(args.constraints);

    if (!args.storage_def->primary_key)
        throw Exception("StorageKeeperMap requires one column in primary key", ErrorCodes::BAD_ARGUMENTS);

    metadata.primary_key = KeyDescription::getKeyFromAST(args.storage_def->primary_key->ptr(), metadata.columns, args.getContext());
    auto primary_key_names = metadata.getColumnsRequiredForPrimaryKey();
    if (primary_key_names.size() != 1)
        throw Exception("StorageKeeperMap requires one column in primary key", ErrorCodes::BAD_ARGUMENTS);

    return std::make_shared<StorageKeeperMap>(
        args.getContext(), args.table_id, metadata, args.query.attach, primary_key_names[0], root_path, create_missing_root_path, keys_limit);
}

}

void registerStorageKeeperMap(StorageFactory & factory)
{
    factory.registerStorage(
        "KeeperMap",
        create,
        {
            .supports_sort_order = true,
            .supports_parallel_insert = true,
        });
}

}
