#define PONY_WANT_ATOMIC_DEFS

#include "scheduler.h"
#include "cpu.h"
#include "mpmcq.h"
#include "../actor/actor.h"
#include "../gc/cycle.h"
#include "../asio/asio.h"
#include "../mem/pool.h"
#include "ponyassert.h"
#include <dtrace.h>
#include <string.h>
#include <stdio.h>
#include "mutemap.h"

#define PONY_SCHED_BATCH 100

static DECLARE_THREAD_FN(run_thread);

typedef enum
{
  SCHED_BLOCK,
  SCHED_UNBLOCK,
  SCHED_CNF,
  SCHED_ACK,
  SCHED_TERMINATE,
  SCHED_UNMUTE_ACTOR
} sched_msg_t;

// Scheduler global data.
static uint32_t scheduler_count;
static scheduler_t* scheduler;
static PONY_ATOMIC(bool) detect_quiescence;
static bool use_yield;
static mpmcq_t inject;
static __pony_thread_local scheduler_t* this_scheduler;

/**
 * Gets the next actor from the scheduler queue.
 */
static pony_actor_t* pop(scheduler_t* sched)
{
  return (pony_actor_t*)ponyint_mpmcq_pop(&sched->q);
}

/**
 * Puts an actor on the scheduler queue.
 */
static void push(scheduler_t* sched, pony_actor_t* actor)
{
  ponyint_mpmcq_push_single(&sched->q, actor);
}

/**
 * Handles the global queue and then pops from the local queue
 */
static pony_actor_t* pop_global(scheduler_t* sched)
{
  pony_actor_t* actor = (pony_actor_t*)ponyint_mpmcq_pop(&inject);

  if(actor != NULL)
    return actor;

  return pop(sched);
}

/**
 * Sends a message to a thread.
 */

static void send_msg(uint32_t to, sched_msg_t msg, intptr_t arg)
{
  pony_msgi_t* m = (pony_msgi_t*)pony_alloc_msg(
    POOL_INDEX(sizeof(pony_msgi_t)), msg);

  m->i = arg;
  ponyint_messageq_push(&scheduler[to].mq, &m->msg, &m->msg);
}

static void send_msg_all(sched_msg_t msg, intptr_t arg)
{
  send_msg(0, msg, arg);

  for(uint32_t i = 1; i < scheduler_count; i++)
    send_msg(i, msg, arg);
}

static void read_msg(scheduler_t* sched)
{
  pony_msgi_t* m;

  while((m = (pony_msgi_t*)ponyint_messageq_pop(&sched->mq)) != NULL)
  {
    switch(m->msg.id)
    {
      case SCHED_BLOCK:
      {
        sched->block_count++;

        if(atomic_load_explicit(&detect_quiescence, memory_order_relaxed) &&
          (sched->block_count == scheduler_count))
        {
          // If we think all threads are blocked, send CNF(token) to everyone.
          send_msg_all(SCHED_CNF, sched->ack_token);
        }
        break;
      }

      case SCHED_UNBLOCK:
      {
        // Cancel all acks and increment the ack token, so that any pending
        // acks in the queue will be dropped when they are received.
        sched->block_count--;
        sched->ack_token++;
        sched->ack_count = 0;
        break;
      }

      case SCHED_CNF:
      {
        // Echo the token back as ACK(token).
        send_msg(0, SCHED_ACK, m->i);
        break;
      }

      case SCHED_ACK:
      {
        // If it's the current token, increment the ack count.
        if(m->i == sched->ack_token)
          sched->ack_count++;
        break;
      }

      case SCHED_TERMINATE:
      {
        sched->terminate = true;
        break;
      }

      case SCHED_UNMUTE_ACTOR:
      {
        ponyint_sched_unmute_senders(&sched->ctx, (pony_actor_t*)m->i, false);
        break;
      }

      default: {}
    }
  }
}


/**
 * If we can terminate, return true. If all schedulers are waiting, one of
 * them will stop the ASIO back end and tell the cycle detector to try to
 * terminate.
 */

static bool quiescent(scheduler_t* sched, uint64_t tsc, uint64_t tsc2)
{
  read_msg(sched);

  if(sched->terminate)
    return true;

  if(sched->ack_count == scheduler_count)
  {
    if(sched->asio_stopped)
    {
      send_msg_all(SCHED_TERMINATE, 0);

      sched->ack_token++;
      sched->ack_count = 0;
    } else if(ponyint_asio_stop()) {
      sched->asio_stopped = true;
      sched->ack_token++;
      sched->ack_count = 0;

      // Run another CNF/ACK cycle.
      send_msg_all(SCHED_CNF, sched->ack_token);
    }
  }

  ponyint_cpu_core_pause(tsc, tsc2, use_yield);
  return false;
}



static scheduler_t* choose_victim(scheduler_t* sched)
{
  scheduler_t* victim = sched->last_victim;

  while(true)
  {
    // Schedulers are laid out sequentially in memory

    // Back up one.
    victim--;

    if(victim < scheduler)
      // victim is before the first scheduler location
      // wrap around to the end.
      victim = &scheduler[scheduler_count - 1];

    if(victim == sched->last_victim)
    {
      // If we have tried all possible victims, return no victim. Set our last
      // victim to ourself to indicate we've started over.
      sched->last_victim = sched;
      break;
    }

    // Don't try to steal from ourself.
    if(victim == sched)
      continue;

    // Record that this is our victim and return it.
    sched->last_victim = victim;
    return victim;
  }

  return NULL;
}


/**
 * Use mpmcqs to allow stealing directly from a victim, without waiting for a
 * response.
 */
static pony_actor_t* steal(scheduler_t* sched, pony_actor_t* prev)
{
  send_msg(0, SCHED_BLOCK, 0);
  uint64_t tsc = ponyint_cpu_tick();
  pony_actor_t* actor;

  while(true)
  {
    scheduler_t* victim = choose_victim(sched);

    if(victim == NULL)
      actor = (pony_actor_t*)ponyint_mpmcq_pop(&inject);
    else
      actor = pop_global(victim);

    if(actor != NULL)
    {
      DTRACE3(WORK_STEAL_SUCCESSFUL, (uintptr_t)sched, (uintptr_t)victim, (uintptr_t)actor);
      break;
    }

    uint64_t tsc2 = ponyint_cpu_tick();

    if(quiescent(sched, tsc, tsc2))
    {
      DTRACE2(WORK_STEAL_FAILURE, (uintptr_t)sched, (uintptr_t)victim);
      return NULL;
    }

    // If we have been passed an actor (implicitly, the cycle detector), and
    // enough time has elapsed without stealing or quiescing, return the actor
    // we were passed (allowing the cycle detector to run).
    if((prev != NULL) && ((tsc2 - tsc) > 10000000000))
    {
      actor = prev;
      break;
    }
  }

  send_msg(0, SCHED_UNBLOCK, 0);
  return actor;
}

/**
 * Run a scheduler thread until termination.
 */
static void run(scheduler_t* sched)
{
  pony_actor_t* actor = pop_global(sched);
  if (DTRACE_ENABLED(ACTOR_SCHEDULED) && actor != NULL) {
    DTRACE2(ACTOR_SCHEDULED, (uintptr_t)sched, (uintptr_t)actor);
  }

  while(true)
  {
    read_msg(sched);

    if(actor == NULL)
    {
      // We had an empty queue and no rescheduled actor.
      actor = steal(sched, NULL);

      if(actor == NULL)
      {
        // Termination.
        pony_assert(pop(sched) == NULL);
        return;
      }
      DTRACE2(ACTOR_SCHEDULED, (uintptr_t)sched, (uintptr_t)actor);
    }

    // Run the current actor and get the next actor.
    bool reschedule = ponyint_actor_run(&sched->ctx, actor, PONY_SCHED_BATCH);
    pony_actor_t* next = pop_global(sched);

    if(reschedule)
    {
      if(next != NULL)
      {
        // If we have a next actor, we go on the back of the queue. Otherwise,
        // we continue to run this actor.
        push(sched, actor);
        DTRACE2(ACTOR_DESCHEDULED, (uintptr_t)sched, (uintptr_t)actor);
        actor = next;
        DTRACE2(ACTOR_SCHEDULED, (uintptr_t)sched, (uintptr_t)actor);
      } else if(ponyint_is_cycle(actor)) {
        // If all we have is the cycle detector, try to steal something else to
        // run as well.
        next = steal(sched, actor);

        if(next == NULL)
        {
          // Termination.
          DTRACE2(ACTOR_DESCHEDULED, (uintptr_t)sched, (uintptr_t)actor);
          return;
        }

        // Push the cycle detector and run the actor we stole.
        if(actor != next)
        {
          push(sched, actor);
          DTRACE2(ACTOR_DESCHEDULED, (uintptr_t)sched, (uintptr_t)actor);
          actor = next;
          DTRACE2(ACTOR_SCHEDULED, (uintptr_t)sched, (uintptr_t)actor);
        }
      }
    } else {
      // We aren't rescheduling, so run the next actor. This may be NULL if our
      // queue was empty.
      DTRACE2(ACTOR_DESCHEDULED, (uintptr_t)sched, (uintptr_t)actor);
      actor = next;
      if (DTRACE_ENABLED(ACTOR_SCHEDULED) && actor != NULL) {
        DTRACE2(ACTOR_SCHEDULED, (uintptr_t)sched, (uintptr_t)actor);
      }
    }
  }
}

static DECLARE_THREAD_FN(run_thread)
{
  scheduler_t* sched = (scheduler_t*) arg;
  this_scheduler = sched;
  ponyint_cpu_affinity(sched->cpu);
  run(sched);
  ponyint_pool_thread_cleanup();

  return 0;
}

static void ponyint_sched_shutdown()
{
  uint32_t start;

  start = 0;

  for(uint32_t i = start; i < scheduler_count; i++)
    ponyint_thread_join(scheduler[i].tid);

  DTRACE0(RT_END);
  ponyint_cycle_terminate(&scheduler[0].ctx);

  for(uint32_t i = 0; i < scheduler_count; i++)
  {
    while(ponyint_messageq_pop(&scheduler[i].mq) != NULL);
    ponyint_messageq_destroy(&scheduler[i].mq);
    ponyint_mpmcq_destroy(&scheduler[i].q);
  }

  ponyint_pool_free_size(scheduler_count * sizeof(scheduler_t), scheduler);
  scheduler = NULL;
  scheduler_count = 0;

  ponyint_mpmcq_destroy(&inject);
}

pony_ctx_t* ponyint_sched_init(uint32_t threads, bool noyield, bool nopin,
  bool pinasio)
{
  pony_register_thread();

  use_yield = !noyield;

  // If no thread count is specified, use the available physical core count.
  if(threads == 0)
    threads = ponyint_cpu_count();

  scheduler_count = threads;
  scheduler = (scheduler_t*)ponyint_pool_alloc_size(
    scheduler_count * sizeof(scheduler_t));
  memset(scheduler, 0, scheduler_count * sizeof(scheduler_t));

  uint32_t asio_cpu = ponyint_cpu_assign(scheduler_count, scheduler, nopin,
    pinasio);

  for(uint32_t i = 0; i < scheduler_count; i++)
  {
    scheduler[i].ctx.scheduler = &scheduler[i];
    scheduler[i].last_victim = &scheduler[i];
    ponyint_messageq_init(&scheduler[i].mq);
    ponyint_mpmcq_init(&scheduler[i].q);
  }

  ponyint_mpmcq_init(&inject);
  ponyint_asio_init(asio_cpu);

  return pony_ctx();
}

bool ponyint_sched_start(bool library)
{
  pony_register_thread();

  if(!ponyint_asio_start())
    return false;

  atomic_store_explicit(&detect_quiescence, !library, memory_order_relaxed);

  DTRACE0(RT_START);
  uint32_t start = 0;

  for(uint32_t i = start; i < scheduler_count; i++)
  {
    if(!ponyint_thread_create(&scheduler[i].tid, run_thread, scheduler[i].cpu,
      &scheduler[i]))
      return false;
  }

  if(!library)
  {
    ponyint_sched_shutdown();
  }

  return true;
}

void ponyint_sched_stop()
{
  atomic_store_explicit(&detect_quiescence, true, memory_order_relaxed);
  ponyint_sched_shutdown();
}

void ponyint_sched_add(pony_ctx_t* ctx, pony_actor_t* actor)
{
  if(ctx->scheduler != NULL)
  {
    // Add to the current scheduler thread.
    push(ctx->scheduler, actor);
  } else {
    // Put on the shared mpmcq.
    ponyint_mpmcq_push(&inject, actor);
  }
}

uint32_t ponyint_sched_cores()
{
  return scheduler_count;
}

PONY_API void pony_register_thread()
{
  if(this_scheduler != NULL)
    return;

  // Create a scheduler_t, even though we will only use the pony_ctx_t.
  this_scheduler = POOL_ALLOC(scheduler_t);
  memset(this_scheduler, 0, sizeof(scheduler_t));
  this_scheduler->tid = ponyint_thread_self();
}

PONY_API void pony_unregister_thread()
{
  if(this_scheduler == NULL)
    return;

  POOL_FREE(scheduler_t, this_scheduler);
  this_scheduler = NULL;

  ponyint_pool_thread_cleanup();
}

PONY_API pony_ctx_t* pony_ctx()
{
  pony_assert(this_scheduler != NULL);
  return &this_scheduler->ctx;
}

void ponyint_sched_mute(pony_ctx_t* ctx, pony_actor_t* sender, pony_actor_t* recv)
{
  scheduler_t* sched = ctx->scheduler;
  size_t index = HASHMAP_UNKNOWN;
  muteref_t key;
  key.key = recv;

  muteref_t* mref = ponyint_mutemap_get(&sched->mute_mapping, &key, &index);
  if(mref == NULL)
  {
    mref = ponyint_muteref_alloc(recv);
    ponyint_mutemap_putindex(&sched->mute_mapping, mref, index);
  }

  size_t index2 = HASHMAP_UNKNOWN;
  pony_actor_t* r = ponyint_muteset_get(&mref->value, sender, &index2);
  if(r == NULL)
  {
    ponyint_muteset_putindex(&mref->value, sender, index2);
    uint8_t muted = atomic_load_explicit(&sender->muted, memory_order_relaxed);
    atomic_store_explicit(&sender->muted, muted + 1, memory_order_relaxed);
  }
}

void ponyint_sched_unmute_senders(pony_ctx_t* ctx, pony_actor_t* actor, bool inform)
{
  scheduler_t* sched = ctx->scheduler;
  size_t index;
  muteref_t key;
  key.key = actor;

  if(inform)
  {
    for(uint32_t i = 0; i < scheduler_count; i++)
    {
      if(&scheduler[i] != sched)
        send_msg(i, SCHED_UNMUTE_ACTOR, (intptr_t)actor);
    }
  }

  muteref_t* mref = ponyint_mutemap_get(&sched->mute_mapping, &key, &index);

  if(mref != NULL)
  {
    size_t i = HASHMAP_UNKNOWN;
    pony_actor_t* muted = NULL;

    while((muted = ponyint_muteset_next(&mref->value, &i)) != NULL)
    {
      uint8_t muted_count = atomic_load_explicit(&muted->muted, memory_order_relaxed);
      pony_assert(muted_count > 0);
      muted_count--;
      atomic_store_explicit(&muted->muted, muted_count, memory_order_relaxed);

      if (muted->muted == 0)
      {
        ponyint_sched_add(ctx, muted);
        ponyint_sched_unmute_senders(ctx, muted, true);
      }
    }

    ponyint_mutemap_removeindex(&sched->mute_mapping, index);
    ponyint_muteref_free(mref);
  }
}

