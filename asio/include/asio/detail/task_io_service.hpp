//
// task_io_service.hpp
  // ~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2006 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_DETAIL_TASK_IO_SERVICE_HPP
#define ASIO_DETAIL_TASK_IO_SERVICE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/push_options.hpp"

#include "asio/detail/push_options.hpp"
#include <memory>
#include "asio/detail/pop_options.hpp"

#include "asio/service_factory.hpp"
#include "asio/detail/bind_handler.hpp"
#include "asio/detail/call_stack.hpp"
#include "asio/detail/event.hpp"
#include "asio/detail/handler_alloc_helpers.hpp"
#include "asio/detail/mutex.hpp"

namespace asio {
namespace detail {

template <typename Task, typename Allocator>
class task_io_service
{
public:
  // Constructor.
  template <typename IO_Service>
  task_io_service(IO_Service& io_service)
    : mutex_(),
      allocator_(io_service.get_allocator()),
      task_(io_service.get_service(service_factory<Task>())),
      outstanding_work_(0),
      handler_queue_(&task_handler_),
      handler_queue_end_(&task_handler_),
      interrupted_(false),
      first_idle_thread_(0)
  {
  }

  // Run the event processing loop.
  void run()
  {
    typename call_stack<task_io_service>::context ctx(this);

    idle_thread_info this_idle_thread;
    this_idle_thread.prev = &this_idle_thread;
    this_idle_thread.next = &this_idle_thread;

    asio::detail::mutex::scoped_lock lock(mutex_);

    while (!interrupted_ && outstanding_work_ > 0)
    {
      if (handler_queue_)
      {
        // Prepare to execute first handler from queue.
        handler_base* h = handler_queue_;
        handler_queue_ = h->next_;
        if (handler_queue_ == 0)
          handler_queue_end_ = 0;
        bool more_handlers = (handler_queue_ != 0);
        lock.unlock();

        if (h == &task_handler_)
        {
          // Helper class to perform operations on block exit.
          class cleanup
          {
          public:
            cleanup(asio::detail::mutex::scoped_lock& lock,
                handler_base*& handler_queue, handler_base*& handler_queue_end,
                handler_base& task_handler)
              : lock_(lock),
                handler_queue_(handler_queue),
                handler_queue_end_(handler_queue_end),
                task_handler_(task_handler)
            {
            }

            ~cleanup()
            {
              // Reinsert the task at the end of the handler queue.
              lock_.lock();
              task_handler_.next_ = 0;
              if (handler_queue_end_)
              {
                handler_queue_end_->next_ = &task_handler_;
                handler_queue_end_ = &task_handler_;
              }
              else
              {
                handler_queue_ = handler_queue_end_ = &task_handler_;
              }
            }

          private:
            asio::detail::mutex::scoped_lock& lock_;
            handler_base*& handler_queue_;
            handler_base*& handler_queue_end_;
            handler_base& task_handler_;
          } c(lock, handler_queue_, handler_queue_end_, task_handler_);

          // Run the task. May throw an exception. Only block if the handler
          // queue is empty, otherwise we want to return as soon as possible to
          // execute the handlers.
          task_.run(!more_handlers);
        }
        else
        {
          // Helper class to perform operations on block exit.
          class cleanup
          {
          public:
            cleanup(asio::detail::mutex::scoped_lock& lock,
                int& outstanding_work)
              : lock_(lock),
                outstanding_work_(outstanding_work)
            {
            }

            ~cleanup()
            {
              lock_.lock();
              --outstanding_work_;
            }

          private:
            asio::detail::mutex::scoped_lock& lock_;
            int& outstanding_work_;
          } c(lock, outstanding_work_);

          // Invoke the handler. May throw an exception.
          h->call(allocator_); // call() deletes the handler object
        }
      }
      else 
      {
        // Nothing to run right now, so just wait for work to do.
        if (first_idle_thread_)
        {
          this_idle_thread.next = first_idle_thread_;
          this_idle_thread.prev = first_idle_thread_->prev;
          first_idle_thread_->prev->next = &this_idle_thread;
          first_idle_thread_->prev = &this_idle_thread;
        }
        first_idle_thread_ = &this_idle_thread;
        this_idle_thread.wakeup_event.clear();
        lock.unlock();
        this_idle_thread.wakeup_event.wait();
        lock.lock();
        if (this_idle_thread.next == &this_idle_thread)
        {
          first_idle_thread_ = 0;
        }
        else
        {
          if (first_idle_thread_ == &this_idle_thread)
            first_idle_thread_ = this_idle_thread.next;
          this_idle_thread.next->prev = this_idle_thread.prev;
          this_idle_thread.prev->next = this_idle_thread.next;
          this_idle_thread.next = &this_idle_thread;
          this_idle_thread.prev = &this_idle_thread;
        }
      }
    }

    if (!interrupted_)
    {
      // No more work to do!
      interrupt_all_threads();
    }
  }

  // Interrupt the event processing loop.
  void interrupt()
  {
    asio::detail::mutex::scoped_lock lock(mutex_);
    interrupt_all_threads();
  }

  // Reset in preparation for a subsequent run invocation.
  void reset()
  {
    asio::detail::mutex::scoped_lock lock(mutex_);
    interrupted_ = false;
  }

  // Notify that some work has started.
  void work_started()
  {
    asio::detail::mutex::scoped_lock lock(mutex_);
    ++outstanding_work_;
  }

  // Notify that some work has finished.
  void work_finished()
  {
    asio::detail::mutex::scoped_lock lock(mutex_);
    if (--outstanding_work_ == 0)
      interrupt_all_threads();
  }

  // Request invocation of the given handler.
  template <typename Handler>
  void dispatch(Handler handler)
  {
    if (call_stack<task_io_service>::contains(this))
      handler();
    else
      post(handler);
  }

  // Request invocation of the given handler and return immediately.
  template <typename Handler>
  void post(Handler handler)
  {
    // Allocate and construct an operation to wrap the handler.
    typedef handler_wrapper<Handler> value_type;
    typedef handler_alloc_traits<Handler, value_type, Allocator> alloc_traits;
    raw_handler_ptr<alloc_traits> raw_ptr(handler, allocator_);
    handler_ptr<alloc_traits> ptr(raw_ptr, handler);

    asio::detail::mutex::scoped_lock lock(mutex_);

    // Add the handler to the end of the queue.
    if (handler_queue_end_)
    {
      handler_queue_end_->next_ = ptr.get();
      handler_queue_end_ = ptr.get();
    }
    else
    {
      handler_queue_ = handler_queue_end_ = ptr.get();
    }
    ptr.release();

    // An undelivered handler is treated as unfinished work.
    ++outstanding_work_;

    // Wake up a thread to execute the handler.
    if (!interrupt_one_idle_thread())
      if (task_handler_.next_ == 0 && handler_queue_end_ != &task_handler_)
        task_.interrupt();
  }

private:
  // Interrupt the task and all idle threads.
  void interrupt_all_threads()
  {
    interrupted_ = true;
    interrupt_all_idle_threads();
    if (task_handler_.next_ == 0 && handler_queue_end_ != &task_handler_)
      task_.interrupt();
  }

  // Interrupt a single idle thread. Returns true if a thread was interrupted,
  // false if no running thread could be found to interrupt.
  bool interrupt_one_idle_thread()
  {
    if (first_idle_thread_)
    {
      first_idle_thread_->wakeup_event.signal();
      first_idle_thread_ = first_idle_thread_->next;
      return true;
    }
    return false;
  }

  // Interrupt all idle threads.
  void interrupt_all_idle_threads()
  {
    if (first_idle_thread_)
    {
      first_idle_thread_->wakeup_event.signal();
      idle_thread_info* current_idle_thread = first_idle_thread_->next;
      while (current_idle_thread != first_idle_thread_)
      {
        current_idle_thread->wakeup_event.signal();
        current_idle_thread = current_idle_thread->next;
      }
    }
  }

  // The base class for all handler wrappers. A function pointer is used
  // instead of virtual functions to avoid the associated overhead.
  class handler_base
  {
  public:
    typedef void (*func_type)(handler_base*, const Allocator&);

    handler_base(func_type func)
      : next_(0),
        func_(func)
    {
    }

    void call(const Allocator& void_allocator)
    {
      func_(this, void_allocator);
    }

  protected:
    // Prevent deletion through this type.
    ~handler_base()
    {
    }

  private:
    friend class task_io_service<Task, Allocator>;
    handler_base* next_;
    func_type func_;
  };

  // Template wrapper for handlers.
  template <typename Handler>
  class handler_wrapper
    : public handler_base
  {
  public:
    handler_wrapper(Handler handler)
      : handler_base(&handler_wrapper<Handler>::do_call),
        handler_(handler)
    {
    }

    static void do_call(handler_base* base, const Allocator& void_allocator)
    {
      // Take ownership of the handler object.
      typedef handler_wrapper<Handler> this_type;
      this_type* h(static_cast<this_type*>(base));
      typedef handler_alloc_traits<Handler, this_type, Allocator> alloc_traits;
      handler_ptr<alloc_traits> ptr(h->handler_, void_allocator, h);

      // Make a copy of the handler so that the memory can be deallocated before
      // the upcall is made.
      Handler handler(h->handler_);

      // Free the memory associated with the handler.
      ptr.reset();

      // Make the upcall.
      handler();
    }

  private:
    Handler handler_;
  };

  // Mutex to protect access to internal data.
  asio::detail::mutex mutex_;

  // The allocator to be used for allocating dynamic objects.
  Allocator allocator_;

  // The task to be run by this service.
  Task& task_;

  // Handler object to represent the position of the task in the queue.
  class task_handler
    : public handler_base
  {
  public:
    task_handler()
      : handler_base(0)
    {
    }
  } task_handler_;

  // The count of unfinished work.
  int outstanding_work_;

  // The start of a linked list of handlers that are ready to be delivered.
  handler_base* handler_queue_;

  // The end of a linked list of handlers that are ready to be delivered.
  handler_base* handler_queue_end_;

  // Flag to indicate that the dispatcher has been interrupted.
  bool interrupted_;

  // Structure containing information about an idle thread.
  struct idle_thread_info
  {
    event wakeup_event;
    idle_thread_info* prev;
    idle_thread_info* next;
  };

  // The number of threads that are currently idle.
  idle_thread_info* first_idle_thread_;
};

} // namespace detail
} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_DETAIL_TASK_IO_SERVICE_HPP