// Copyright (c) 2019 by Robert Bosch GmbH. All rights reserved.
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

#include <time.h>

#include "iceoryx_posh/popo/subscriber.hpp"

#include "rcutils/error_handling.h"

#include "rmw/impl/cpp/macros.hpp"
#include "rmw/rmw.h"

#include "./types/iceoryx_guard_condition.hpp"
#include "./types/iceoryx_subscription.hpp"
#include "./types/iceoryx_wait_set.hpp"

extern "C"
{
rmw_ret_t
rmw_wait(
  rmw_subscriptions_t * subscriptions,
  rmw_guard_conditions_t * guard_conditions,
  rmw_services_t * services,
  rmw_clients_t * clients,
  rmw_events_t * events,
  rmw_wait_set_t * wait_set,
  const rmw_time_t * wait_timeout)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(subscriptions, RMW_RET_ERROR);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(guard_conditions, RMW_RET_ERROR);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(services, RMW_RET_ERROR);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(clients, RMW_RET_ERROR);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(events, RMW_RET_ERROR);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(wait_set, RMW_RET_ERROR);

  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    rmw_wait
    : waitset, wait_set->implementation_identifier,
    rmw_get_implementation_identifier(), return RMW_RET_ERROR);

  auto iceoryx_wait_set = static_cast<IceoryxWaitSet *>(wait_set->data);
  if (!iceoryx_wait_set) {
    return RMW_RET_ERROR;
  }

  iox::posix::Semaphore * semaphore = iceoryx_wait_set->semaphore_;
  if (!semaphore) {
    return RMW_RET_ERROR;
  }

  // attach semaphore to all iceoryx receivers
  for (size_t i = 0; i < subscriptions->subscriber_count; ++i) {
    auto iceoryx_subscription =
      static_cast<IceoryxSubscription * const>(subscriptions->subscribers[i]);
    auto iceoryx_receiver = iceoryx_subscription->iceoryx_receiver_;

    // indicate that we do not have to wait if there is already a new sample
    if (iceoryx_receiver->hasNewChunks()) {
      goto after_wait;
    }

    iceoryx_receiver->setChunkReceiveSemaphore(semaphore);
  }

  // attach semaphore to all guard conditions
  for (size_t i = 0; i < guard_conditions->guard_condition_count; ++i) {
    auto iceoryx_guard_condition =
      static_cast<IceoryxGuardCondition * const>(guard_conditions->guard_conditions[i]);

    // indicate that we do not have to wait if there is already a triggered guard condition
    if (iceoryx_guard_condition->hasTriggered()) {
      goto after_wait;
    }

    iceoryx_guard_condition->attachSemaphore(semaphore);
  }

  if (!wait_timeout) {
    semaphore->wait();
  } else {
    struct timespec ts_start;
    struct timespec ts_end;
    clock_gettime(CLOCK_REALTIME, &ts_start);
    uint64_t nsec = ts_start.tv_nsec + wait_timeout->nsec;
    if (nsec >= 1000000000L) {
      nsec -= 1000000000L;
      ts_end.tv_sec = ts_start.tv_sec + wait_timeout->sec + 1;
    } else {
      ts_end.tv_sec = ts_start.tv_sec + wait_timeout->sec;
    }
    ts_end.tv_nsec = nsec;
    semaphore->timedWait(&ts_end, true);

    // // for debugging
    // struct timespec ts_diff;
    // clock_gettime(CLOCK_REALTIME, &ts_diff);
    // int64_t diff_nsec = ts_diff.tv_nsec - ts_start.tv_nsec;
    // if (diff_nsec < 0L) {
    //   diff_nsec += 1000000000L;
    //   ts_diff.tv_sec = ts_diff.tv_sec - ts_start.tv_sec - 1;
    // } else {
    //   ts_diff.tv_sec = ts_diff.tv_sec - ts_start.tv_sec;
    // }
    // ts_diff.tv_nsec = diff_nsec;
    // printf("waited s %lu ns %lu\n", ts_diff.tv_sec, ts_diff.tv_nsec);
  }

after_wait:


  // reset all the subscriptions that don't have new data
  for (size_t i = 0; i < subscriptions->subscriber_count; ++i) {
    auto iceoryx_subscription =
      static_cast<IceoryxSubscription * const>(subscriptions->subscribers[i]);
    iox::popo::Subscriber * iceoryx_receiver = iceoryx_subscription->iceoryx_receiver_;

    // remove semaphore from all receivers because next call a new waitset could be provided
    iceoryx_receiver->unsetChunkReceiveSemaphore();

    if (!iceoryx_receiver->hasNewChunks()) {
      subscriptions->subscribers[i] = nullptr;
    }
  }

  // reset all the guard_conditions that have not triggered
  for (size_t i = 0; i < guard_conditions->guard_condition_count; ++i) {
    auto iceoryx_guard_condition =
      static_cast<IceoryxGuardCondition * const>(guard_conditions->guard_conditions[i]);

    iceoryx_guard_condition->detachSemaphore();

    if (iceoryx_guard_condition->hasTriggered()) {
      iceoryx_guard_condition->resetTriggerIndication();
    } else {
      guard_conditions->guard_conditions[i] = nullptr;
    }
  }

  // clear the semaphore
  // events that triggered it and where not yet collected will be seen on next rmw_wait
  while (semaphore->tryWait()) {
  }

  return RMW_RET_OK;
}
}  // extern "C"
