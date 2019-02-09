//
//
// Copyright 2018 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//

#include <grpc/support/port_platform.h>

#include "src/core/ext/filters/client_channel/global_subchannel_pool.h"

#include "src/core/ext/filters/client_channel/backup_poller.h"
#include "src/core/ext/filters/client_channel/subchannel.h"
#include "src/core/lib/gpr/env.h"
#include "src/core/lib/gpr/string.h"

// TODO(dgq): When we C++-ify the relevant code, we need to make sure that any
// static variable in this file is trivially-destructible.

// If a subchannel only has one external ref left, which is held by the
// subchannel index, it is not used by any other external user (typically, LB
// policy). Instead of unregistering a subchannel once it's unused, the
// subchannel index will periodically sweep these unused subchannels, like a
// garbage collector. This mechanism can alleviate subchannel
// registration/unregistration churn. The subchannel can keep unchanged if it's
// re-used shortly after it's unused, which is desirable in the gRPC LB use
// case.
constexpr grpc_millis kDefaultSweepIntervalMs = 1000;
// This number was picked pseudo-randomly and could probably be tuned for
// performance reasons.

namespace grpc_core {

class GlobalSubchannelPool::Sweeper : public InternallyRefCounted<Sweeper> {
 public:
  Sweeper(GlobalSubchannelPool* subchannel_pool =
              GlobalSubchannelPool::instance().get())
      : subchannel_pool_(std::move(subchannel_pool)) {
    char* sweep_interval_env =
        gpr_getenv("GRPC_SUBCHANNEL_INDEX_SWEEP_INTERVAL_MS");
    if (sweep_interval_env != nullptr) {
      int sweep_interval_ms = gpr_parse_nonnegative_int(sweep_interval_env);
      if (sweep_interval_ms == -1) {
        gpr_log(GPR_ERROR,
                "Invalid GRPC_SUBCHANNEL_INDEX_SWEEP_INTERVAL_MS: %s, default "
                "value %d will be used.",
                sweep_interval_env, static_cast<int>(sweep_interval_ms_));
      } else {
        sweep_interval_ms_ = static_cast<grpc_millis>(sweep_interval_ms);
      }
      gpr_free(sweep_interval_env);
    }
    GRPC_CLOSURE_INIT(&sweep_unused_subchannels_, SweepUnusedSubchannels, this,
                      grpc_schedule_on_exec_ctx);
    ScheduleNextSweep();
  }

  void Orphan() override {
    gpr_atm_no_barrier_store(&shutdown_, 0);
    grpc_timer_cancel(&sweeper_timer_);
  }

 private:
  void ScheduleNextSweep() {
    const grpc_millis next_sweep_time =
        ::grpc_core::ExecCtx::Get()->Now() + sweep_interval_ms_;
    grpc_timer_init(&sweeper_timer_, next_sweep_time,
                    &sweep_unused_subchannels_);
    gpr_log(GPR_ERROR, "=== scheduled");
  }

  static void FindUnusedSubchannelsLocked(
      grpc_avl_node* avl_node,
      grpc_core::InlinedVector<Subchannel*, kUnusedSubchannelsInlinedSize>*
          unused_subchannels) {
    if (avl_node == nullptr) return;
    Subchannel* c = static_cast<Subchannel*>(avl_node->value);
    if (c->IsUnused()) unused_subchannels->emplace_back(c);
    FindUnusedSubchannelsLocked(avl_node->left, unused_subchannels);
    FindUnusedSubchannelsLocked(avl_node->right, unused_subchannels);
  }

  static void SweepUnusedSubchannels(void* arg, grpc_error* error) {
    gpr_log(GPR_ERROR, "=== start sweep");
    Sweeper* sweeper = static_cast<Sweeper*>(arg);
    if (error != GRPC_ERROR_NONE ||
        gpr_atm_no_barrier_load(&sweeper->shutdown_)) {
      Delete(sweeper);
      return;
    }
    GlobalSubchannelPool* subchannel_pool = sweeper->subchannel_pool_;
    grpc_core::InlinedVector<Subchannel*, kUnusedSubchannelsInlinedSize>
        unused_subchannels;
    gpr_mu_lock(&subchannel_pool->mu_);
    // We use two-phase cleanup because modification during traversal is unsafe
    // for an AVL tree.
    FindUnusedSubchannelsLocked(subchannel_pool->subchannel_map_.root,
                                &unused_subchannels);
    gpr_mu_unlock(&subchannel_pool->mu_);
    subchannel_pool->UnregisterUnusedSubchannels(unused_subchannels);
    sweeper->ScheduleNextSweep();
  }

  grpc_millis sweep_interval_ms_ = kDefaultSweepIntervalMs;
  grpc_timer sweeper_timer_;
  gpr_atm shutdown_;
  grpc_closure sweep_unused_subchannels_;
  GlobalSubchannelPool* subchannel_pool_;
};

GlobalSubchannelPool::GlobalSubchannelPool() {
  grpc_core::ExecCtx exec_ctx;
  subchannel_map_ = grpc_avl_create(&subchannel_avl_vtable_);
  gpr_mu_init(&mu_);
  // Maybe start backup polling.
  char* s = gpr_getenv("GRPC_POLL_STRATEGY");
  if (s == nullptr || strcmp(s, "none") != 0) {
    pollset_set_ = grpc_pollset_set_create();
    grpc_client_channel_start_backup_polling(pollset_set_);
  }
  gpr_free(s);
  // Set up the subchannel sweeper.
  sweeper_ = MakeOrphanable<Sweeper>(this);
}

GlobalSubchannelPool::~GlobalSubchannelPool() {
  gpr_log(GPR_ERROR, "=== dtor");
  bool shutting_down = true;
  grpc_avl_unref(subchannel_map_, &shutting_down);
  gpr_mu_destroy(&mu_);
}

void GlobalSubchannelPool::Init() {
  instance_ = New<RefCountedPtr<GlobalSubchannelPool>>(
      MakeRefCounted<GlobalSubchannelPool>());
}

void GlobalSubchannelPool::Shutdown() {
  gpr_log(GPR_ERROR, "=== shutdown");
  // To ensure Init() was called before.
  GPR_ASSERT(instance_ != nullptr);
  // To ensure Shutdown() was not called before.
  GPR_ASSERT(*instance_ != nullptr);
  (*instance_)->sweeper_.reset();
  instance_->reset();
  if (pollset_set_ != nullptr) {
    grpc_client_channel_stop_backup_polling(pollset_set_);
    grpc_pollset_set_destroy(pollset_set_);
  }
  // Some subchannels might have been unregistered and disconnected during
  // shutdown time. We should flush the closures before we wait for the iomgr
  // objects to be freed.
  grpc_core::ExecCtx::Get()->Flush();
  Delete(instance_);
}

RefCountedPtr<GlobalSubchannelPool> GlobalSubchannelPool::instance() {
  GPR_ASSERT(instance_ != nullptr);
  GPR_ASSERT(*instance_ != nullptr);
  return *instance_;
}

Subchannel* GlobalSubchannelPool::RegisterSubchannel(
    SubchannelKey* key, Subchannel* constructed) {
  Subchannel* c = nullptr;
  bool shutting_down = false;
  // Compare and swap (CAS) loop:
  while (c == nullptr) {
    // Ref the shared map to have a local copy.
    gpr_mu_lock(&mu_);
    grpc_avl old_map = grpc_avl_ref(subchannel_map_, &shutting_down);
    gpr_mu_unlock(&mu_);
    // Check to see if a subchannel already exists.
    c =
        static_cast<Subchannel*>(grpc_avl_get(old_map, key, &shutting_down));
    if (c != nullptr) {
      // The subchannel already exists. Reuse it.
      GRPC_SUBCHANNEL_REF(c, "index_register_reuse");
      GRPC_SUBCHANNEL_UNREF(constructed, "index_register_found_existing");
      // Exit the CAS loop without modifying the shared map.
    } else {
      // There hasn't been such subchannel. Add one.
      // Note that we should ref the old map first because grpc_avl_add() will
      // unref it while we still need to access it later.
      grpc_avl new_map = grpc_avl_add(
          grpc_avl_ref(old_map, &shutting_down), New<SubchannelKey>(*key),
          GRPC_SUBCHANNEL_REF(constructed, "index_register_new"), &shutting_down);
      // Try to publish the change to the shared map. It may happen (but
      // unlikely) that some other thread has changed the shared map, so compare
      // to make sure it's unchanged before swapping. Retry if it's changed.
      gpr_mu_lock(&mu_);
      if (old_map.root == subchannel_map_.root) {
        GPR_SWAP(grpc_avl, new_map, subchannel_map_);
        c = constructed;
        grpc_pollset_set_add_pollset_set(c->pollset_set(),
                                         pollset_set_);
      }
      gpr_mu_unlock(&mu_);
      grpc_avl_unref(new_map, &shutting_down);
    }
    grpc_avl_unref(old_map, &shutting_down);
  }
  return c;
}

void GlobalSubchannelPool::UnregisterSubchannel(SubchannelKey* key) { abort(); }

Subchannel* GlobalSubchannelPool::FindSubchannel(
    SubchannelKey* key) {
  bool shutting_down = false;
  // Lock, and take a reference to the subchannel map.
  // We don't need to do the search under a lock as AVL's are immutable.
  gpr_mu_lock(&mu_);
  grpc_avl index = grpc_avl_ref(subchannel_map_, &shutting_down);
  gpr_mu_unlock(&mu_);
  Subchannel* c =
      static_cast<Subchannel*>(grpc_avl_get(index, key, &shutting_down));
  if (c != nullptr) GRPC_SUBCHANNEL_REF(c, "index_find");
  grpc_avl_unref(index, &shutting_down);
  return c;
}

void GlobalSubchannelPool::TestOnlyStopSweep() {
  //  // For cancelling timer.
  //  grpc_core::ExecCtx exec_ctx;
  (*instance_)->sweeper_.reset();
}

void GlobalSubchannelPool::TestOnlyStartSweep() {
  //  grpc_core::ExecCtx exec_ctx;
  (*instance_)->sweeper_ = MakeOrphanable<Sweeper>();
}

void GlobalSubchannelPool::UnregisterUnusedSubchannels(
    const grpc_core::InlinedVector<Subchannel*, kUnusedSubchannelsInlinedSize>&
        unused_subchannels) {
  bool shutting_down = false;
  for (size_t i = 0; i < unused_subchannels.size(); ++i) {
    Subchannel* c = unused_subchannels[i];
    SubchannelKey* key = c->key();
    bool done = false;
    // Compare and swap (CAS) loop:
    while (!done) {
      // Ref the shared map to have a local copy.
      gpr_mu_lock(&mu_);
      grpc_avl old_map = grpc_avl_ref(subchannel_map_, &shutting_down);
      gpr_mu_unlock(&mu_);
      // Remove the subchannel.
      // Note that we should ref the old map first because grpc_avl_remove()
      // will unref it while we still need to access it later.
      grpc_avl new_map = grpc_avl_remove(grpc_avl_ref(old_map, &shutting_down),
                                         key, &shutting_down);
      // Try to publish the change to the shared map. It may happen (but
      // unlikely) that some other thread has changed the shared map, so compare
      // to make sure it's unchanged before swapping. Retry if it's changed.
      gpr_mu_lock(&mu_);
      if (old_map.root == subchannel_map_.root) {
        GPR_SWAP(grpc_avl, new_map, subchannel_map_);
        done = true;
      }
      gpr_mu_unlock(&mu_);
      grpc_avl_unref(new_map, &shutting_down);
      grpc_avl_unref(old_map, &shutting_down);
    }
  }
}

RefCountedPtr<GlobalSubchannelPool>* GlobalSubchannelPool::instance_ = nullptr;

grpc_pollset_set* GlobalSubchannelPool::pollset_set_ = nullptr;

namespace {

void sck_avl_destroy(void* p, void* user_data) {
  SubchannelKey* key = static_cast<SubchannelKey*>(p);
  Delete(key);
}

void* sck_avl_copy(void* p, void* unused) {
  const SubchannelKey* key = static_cast<const SubchannelKey*>(p);
  auto* new_key = New<SubchannelKey>(*key);
  return static_cast<void*>(new_key);
}

long sck_avl_compare(void* a, void* b, void* unused) {
  const SubchannelKey* key_a = static_cast<const SubchannelKey*>(a);
  const SubchannelKey* key_b = static_cast<const SubchannelKey*>(b);
  return key_a->Cmp(*key_b);
}

void scv_avl_destroy(void* p, void* user_data) {
  Subchannel* c = static_cast<Subchannel*>(p);
  GRPC_SUBCHANNEL_UNREF(c,
                        "subchannel_index_scv_avl_destroy");
  // Shutting down.
  if (static_cast<bool>(user_data)) {
    grpc_pollset_set_del_pollset_set(
        c->pollset_set(), GlobalSubchannelPool::instance()->pollset_set());
  }
}

void* scv_avl_copy(void* p, void* unused) {
  Subchannel* c = static_cast<Subchannel*>(p);
  GRPC_SUBCHANNEL_REF(c, "subchannel_index_scv_avl_copy");
  return p;
}

}  // namespace

const grpc_avl_vtable GlobalSubchannelPool::subchannel_avl_vtable_ = {
    sck_avl_destroy,  // destroy_key
    sck_avl_copy,     // copy_key
    sck_avl_compare,  // compare_keys
    scv_avl_destroy,  // destroy_value
    scv_avl_copy      // copy_value
};

}  // namespace grpc_core
