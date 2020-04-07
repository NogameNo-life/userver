#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/signals2/signal.hpp>

#include <engine/condition_variable_any.hpp>
#include <engine/deadline.hpp>
#include <engine/ev/thread_control.hpp>
#include <engine/ev/thread_pool.hpp>
#include <utils/swappingsmart.hpp>

#include <storages/redis/impl/command.hpp>
#include <storages/redis/impl/redis_stats.hpp>
#include <storages/redis/impl/wait_connected_mode.hpp>
#include "ev_wrapper.hpp"
#include "keys_for_shards.hpp"
#include "keyshard_impl.hpp"
#include "redis.hpp"
#include "sentinel_query.hpp"
#include "shard.hpp"

namespace redis {

class SentinelImpl {
 public:
  using ReadyChangeCallback = std::function<void(
      size_t shard, const std::string& shard_name, bool master, bool ready)>;

  SentinelImpl(const engine::ev::ThreadControl& sentinel_thread_control,
               const std::shared_ptr<engine::ev::ThreadPool>& redis_thread_pool,
               Sentinel& sentinel, const std::vector<std::string>& shards,
               const std::vector<ConnectionInfo>& conns,
               std::string shard_group_name, const std::string& client_name,
               const std::string& password, ReadyChangeCallback ready_callback,
               std::unique_ptr<KeyShard>&& key_shard, bool track_masters,
               bool track_slaves);
  ~SentinelImpl();

  std::unordered_map<ServerId, size_t, ServerIdHasher>
  GetAvailableServersWeighted(size_t shard_idx, bool with_master,
                              const CommandControl& cc = {}) const;

  void WaitConnectedDebug(bool allow_empty_slaves);

  void WaitConnectedOnce(RedisWaitConnected wait_connected);

  void ForceUpdateHosts();

  static constexpr size_t unknown_shard =
      std::numeric_limits<std::size_t>::max();

  struct SentinelCommand {
    CommandPtr command;
    bool master = true;
    size_t shard = unknown_shard;
    std::chrono::steady_clock::time_point start;

    SentinelCommand() {}
    SentinelCommand(CommandPtr command, bool master, size_t shard,
                    std::chrono::steady_clock::time_point start)
        : command(command), master(master), shard(shard), start(start) {}
  };

  void AsyncCommand(const SentinelCommand& scommand,
                    size_t prev_instance_idx = -1);
  size_t ShardByKey(const std::string& key) const;
  size_t ShardsCount() const { return master_shards_.size(); }
  const std::string& GetAnyKeyForShard(size_t shard_idx) const;
  SentinelStatistics GetStatistics() const;

  void Init();
  void Start();
  void Stop();

  std::vector<std::shared_ptr<const Shard>> GetMasterShards() const;

 private:
  static constexpr const std::chrono::milliseconds cluster_slots_timeout_ =
      std::chrono::milliseconds(4000);

  class SlotInfo {
   public:
    struct ShardInterval {
      size_t slot_min;
      size_t slot_max;
      size_t shard;

      ShardInterval(size_t slot_min, size_t slot_max, size_t shard)
          : slot_min(slot_min), slot_max(slot_max), shard(shard) {}
    };

    SlotInfo();
    ~SlotInfo() = default;

    size_t ShardBySlot(size_t slot) const;
    void UpdateSlots(const std::vector<ShardInterval>& intervals);

   private:
    struct SlotShard {
      size_t bound;
      size_t shard;

      SlotShard(size_t bound, size_t shard) : bound(bound), shard(shard) {}
    };

    std::vector<SlotShard> slot_shards_;
    mutable std::mutex mutex_;
  };

  class ShardInfo {
   public:
    using HostPortToShardMap = std::map<std::pair<std::string, size_t>, size_t>;

    size_t GetShard(const std::string& host, int port) const;
    void UpdateHostPortToShard(HostPortToShardMap&& host_port_to_shard_new);

   private:
    HostPortToShardMap host_port_to_shard_;
    mutable std::mutex mutex_;
  };

  class ConnectedStatus {
   public:
    void SetMasterReady();
    void SetSlaveReady();

    bool WaitReady(engine::Deadline deadline, WaitConnectedMode mode);

   private:
    template <typename Pred>
    bool Wait(engine::Deadline deadline, const Pred& pred);

    std::mutex mutex_;
    std::atomic<bool> master_ready_{false};
    std::atomic<bool> slave_ready_{false};

    engine::impl::ConditionVariableAny<std::mutex> cv_;
  };

  void GenerateKeysForShards(size_t max_len = 4);
  void AsyncCommandFailed(const SentinelCommand& scommand);

  static void OnCheckTimer(struct ev_loop*, ev_timer* w, int revents) noexcept;
  static void ChangedState(struct ev_loop*, ev_async* w, int revents) noexcept;
  static void UpdateInstances(struct ev_loop*, ev_async* w,
                              int revents) noexcept;
  static void OnModifyConnectionInfo(struct ev_loop*, ev_async* w,
                                     int revents) noexcept;

  void ProcessCreationOfShards(bool track, bool master,
                               std::vector<std::shared_ptr<Shard>>& shards);

  void OnCheckTimerImpl();
  void ReadSentinels();
  void CheckConnections();
  void UpdateInstancesImpl();
  ConnInfoMap ConvertConnectionInfoVectorToMap(
      const std::vector<ConnectionInfoInt>& array);
  bool SetConnectionInfo(ConnInfoMap info_by_shards,
                         std::vector<std::shared_ptr<Shard>>& shards,
                         bool master);
  void EnqueueCommand(const SentinelCommand& command);
  size_t ParseMovedShard(const std::string& err_string);
  void UpdateClusterSlots(size_t shard);
  void InitShards(const std::vector<std::string>& shards,
                  std::vector<std::shared_ptr<Shard>>& shard_objects,
                  const ReadyChangeCallback& ready_callback, bool master);

  static size_t HashSlot(const std::string& key);

  void ProcessWaitingCommands();

  Sentinel& sentinel_obj_;
  engine::ev::ThreadControl ev_thread_control_;

  std::string shard_group_name_;
  std::vector<std::string> init_shards_;
  std::vector<std::unique_ptr<ConnectedStatus>> connected_statuses_;
  std::vector<ConnectionInfo> conns_;
  ReadyChangeCallback ready_callback_;

  std::shared_ptr<engine::ev::ThreadPool> redis_thread_pool_;
  struct ev_loop* loop_ = nullptr;
  ev_async watch_state_{};
  ev_async watch_update_{};
  ev_async watch_create_{};
  ev_timer check_timer_{};
  mutable std::mutex sentinels_mutex_;
  std::vector<std::shared_ptr<Shard>> master_shards_;
  std::vector<std::shared_ptr<Shard>> slaves_shards_;
  ConnInfoByShard master_shards_info_;
  ConnInfoByShard slaves_shards_info_;
  std::shared_ptr<Shard> sentinels_;
  std::map<std::string, size_t> shards_;
  ShardInfo shard_info_;
  std::string client_name_;
  std::string password_;
  double check_interval_;
  bool track_masters_;
  bool track_slaves_;
  SlotInfo slot_info_;
  std::vector<SentinelCommand> commands_;
  std::mutex command_mutex_;
  size_t current_slots_shard_ = 0;
  std::unique_ptr<KeyShard> key_shard_;
  SentinelStatisticsInternal statistics_internal_;
  utils::SwappingSmart<KeysForShards> keys_for_shards_;
};

}  // namespace redis
