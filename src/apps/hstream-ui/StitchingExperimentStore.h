#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "src/apps/hstream-ui/StitchingExperimentBackend.h"

inline constexpr size_t kMaximumStoredStitchingExperiments = 1024;
inline constexpr size_t kMaximumStitchingExperimentCatalogBytes = 4 * 1024 * 1024;

struct StitchingExperimentStore {
  std::filesystem::path directory;
  std::filesystem::path game_directory;
  // Open reports interrupted explicit discard without resuming it. Keep the
  // Discard action available; ordinary catalog operations reject pending data.
  bool discard_pending{false};
};

struct StoredStitchingExperiment {
  StitchingExperimentWorkspace workspace;
  // queued, running, scan, frozen, complete, failed or quarantined. This records
  // intent, not proof that an old process has stopped or artifacts are complete.
  std::string state;
  std::string selection_fingerprint;
  std::string artifact_generation_id;
  int64_t process_session_id{0};
  std::string process_token;
  std::string reservation_token;
  // Queue/dependency sequence numbers are local to workspace.root (session).
  int sequence{0};
  int baseline_sequence{0};
  int selection_owner_sequence{0};
  std::string baseline_workspace_key;
  std::string selection_owner_workspace_key;
  std::string saved_selection_fingerprint;
  int scan_duration_seconds{60};
  bool requires_ice_mask{false};
  std::string failure;
  // Zero creates a record; successful Save updates this value. Stale updates
  // fail rather than overwriting another dialog's more recent row state.
  uint64_t revision{0};
};

struct StitchingExperimentCatalog {
  std::vector<StoredStitchingExperiment> experiments;
  // Count -> sessions/<session-id>/<candidate-game-id>, never an absolute path.
  std::map<int, std::string> selected_by_count;
  // Read-only fallback for counts without a selected owner or reservation:
  // the most recently inserted main-derived frozen row, including queued rows.
  // Process ownership and input integrity still require validation before reuse.
  std::map<int, std::string> retained_by_count;
};

// Call these outside artifact/config transactions. Only the catalog lock is
// acquired; config validation reads bounded atomically published snapshots.
// All experiment data stays inside the selected game until explicit discard.
absl::StatusOr<StitchingExperimentStore> OpenStitchingExperimentStore(const std::filesystem::path& game_directory);
absl::StatusOr<StitchingExperimentCatalog> LoadStitchingExperimentStore(const StitchingExperimentStore& store);
// A main-derived row remains durable if main changes before completion; a
// superseded authoritative_main_fingerprint only fills an unclaimed count when
// main moved to another count. Existing defaults/reservations remain unchanged.
// The supplied fingerprint must match the row itself.
absl::Status SaveStitchingExperiment(
    const StitchingExperimentStore& store,
    StoredStitchingExperiment& experiment,
    bool publish_selection = false,
    const std::string& authoritative_main_fingerprint = {});

// The caller must prove this exact persisted process session/token has stopped.
// Reconciles only process intent and failure status through revision CAS; retains
// selections/reservations without reading possibly corrupt workspace config.
absl::Status MarkStitchingExperimentProcessStopped(
    const StitchingExperimentStore& store,
    StoredStitchingExperiment& experiment,
    const std::string& failure);

// A reservation prevents duplicate baseline/scan work for a new count. Its token
// is a group nonce, independent of individual runner process/session tokens.
// Returns the existing frozen record when this count has already been selected,
// including a retained main snapshot saved after a dialog queued its scan;
// nullopt means this owner now holds an idempotent reservation. Another live or
// unconfirmed owner conflicts until explicitly released after shutdown proof.
absl::StatusOr<std::optional<StoredStitchingExperiment>> ReserveStitchingExperimentFrameCount(
    const StitchingExperimentStore& store,
    int frame_count,
    uint64_t anchor_ns,
    const StitchingExperimentWorkspace& owner_workspace,
    const std::string& reservation_token);
absl::Status ReleaseStitchingExperimentFrameCount(
    const StitchingExperimentStore& store,
    int frame_count,
    const StitchingExperimentWorkspace& owner_workspace,
    const std::string& reservation_token);

// The UI must first obtain explicit discard confirmation and prove shutdown.
// Persisted active/quarantined processes block deletion. Removes only the owned
// experiment directory; promoted main-game bundles/artifacts remain untouched.
absl::Status DiscardStitchingExperimentStore(const StitchingExperimentStore& store);

// Explicit removal of queued or failed rows and their private files. Refuses
// active/quarantined process intent and surviving dependencies. Failed rows'
// frozen count selections/reservations are forgotten atomically; queued owners
// must release their reservations first. Main data is untouched.
absl::Status RemoveStitchingExperiments(
    const StitchingExperimentStore& store,
    const std::vector<std::string>& workspace_keys);
