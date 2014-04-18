// Copyright 2010-2012 RethinkDB, all rights reserved.
#ifndef RPC_MAILBOX_MAILBOX_HPP_
#define RPC_MAILBOX_MAILBOX_HPP_

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "containers/archive/archive.hpp"
#include "containers/archive/vector_stream.hpp"
#include "rpc/connectivity/cluster.hpp"
#include "rpc/semilattice/joins/macros.hpp"

class mailbox_manager_t;

/* `mailbox_t` is a receiver of messages. Construct it with a callback function
to handle messages it receives. To send messages to the mailbox, call the
`get_address()` method and then call `send()` on the address it returns. */

class mailbox_write_callback_t {
public:
    virtual ~mailbox_write_callback_t() { }
    virtual void write(write_message_t *msg) = 0;
};

class mailbox_read_callback_t {
public:
    virtual ~mailbox_read_callback_t() { }

    virtual void read(read_stream_t *stream) = 0;
    // Must return a pointer to a `std::function<void(args)>` object, where `args`
    // are the argument types of the mailbox.
    virtual const void *get_local_delivery_cb() = 0;
};

struct raw_mailbox_t : public home_thread_mixin_t {
public:
    struct address_t;
    typedef uint64_t id_t;

private:
    friend class mailbox_manager_t;
    friend class raw_mailbox_writer_t;
    friend void send(mailbox_manager_t *, address_t, mailbox_write_callback_t *);

    mailbox_manager_t *manager;

    const id_t mailbox_id;

    mailbox_read_callback_t *callback;

    auto_drainer_t drainer;

    DISABLE_COPYING(raw_mailbox_t);

public:
    struct address_t {

        /* Constructs a nil address */
        address_t();

        address_t(const address_t&);

        /* Tests if the address is nil */
        bool is_nil() const;

        /* Returns the peer on which the mailbox lives. If the address is nil,
        fails. */
        peer_id_t get_peer() const;

        // Returns a friendly human-readable peer:thread:mailbox_id string.
        std::string human_readable() const;

        RDB_MAKE_ME_EQUALITY_COMPARABLE_3(raw_mailbox_t::address_t, peer, thread, mailbox_id);

    private:
        friend void send(mailbox_manager_t *, raw_mailbox_t::address_t, mailbox_write_callback_t *callback);
        friend struct raw_mailbox_t;
        friend class mailbox_manager_t;

        RDB_MAKE_ME_SERIALIZABLE_3(peer, thread, mailbox_id);

        /* The peer on which the mailbox is located */
        peer_id_t peer;

        /* The thread on `peer` that the mailbox lives on */
        static const int32_t ANY_THREAD;
        int32_t thread;

        /* The ID of the mailbox */
        id_t mailbox_id;
    };

    raw_mailbox_t(mailbox_manager_t *, mailbox_read_callback_t *callback);
    ~raw_mailbox_t();

    address_t get_address() const;
};

/* `send()` sends a message to a mailbox. It is safe to call `send()` outside of
a coroutine; it does not block. If the mailbox does not exist or the peer is
inaccessible, `send()` will silently fail. */

void send(mailbox_manager_t *src,
          raw_mailbox_t::address_t dest,
          mailbox_write_callback_t *callback);

/* `mailbox_manager_t` uses a `message_service_t` to provide mailbox capability.
Usually you will split a `message_service_t` into several sub-services using
`message_multiplexer_t` and put a `mailbox_manager_t` on only one of them,
because the `mailbox_manager_t` relies on something else to send the initial
mailbox addresses back and forth between nodes. */

class mailbox_manager_t : public message_handler_t {
public:
    explicit mailbox_manager_t(message_service_t *);

    /* Returns the connectivity service of the underlying message service. */
    connectivity_service_t *get_connectivity_service() {
        return message_service->get_connectivity_service();
    }

    template<class... arg_ts>
    bool try_local_delivery(const raw_mailbox_t::address_t &dest,
                            const arg_ts&... data);

private:
    friend struct raw_mailbox_t;
    friend void send(mailbox_manager_t *, raw_mailbox_t::address_t, mailbox_write_callback_t *callback);

    message_service_t *message_service;

    struct mailbox_table_t {
        mailbox_table_t();
        ~mailbox_table_t();
        raw_mailbox_t::id_t next_mailbox_id;
        // TODO: use a buffered structure to reduce dynamic allocation
        std::map<raw_mailbox_t::id_t, raw_mailbox_t *> mailboxes;
        raw_mailbox_t *find_mailbox(raw_mailbox_t::id_t);
    };
    one_per_thread_t<mailbox_table_t> mailbox_tables;

    raw_mailbox_t::id_t generate_mailbox_id();

    raw_mailbox_t::id_t register_mailbox(raw_mailbox_t *mb);
    void unregister_mailbox(raw_mailbox_t::id_t id);

    static void write_mailbox_message(write_stream_t *stream,
                                      threadnum_t dest_thread,
                                      raw_mailbox_t::id_t dest_mailbox_id,
                                      mailbox_write_callback_t *callback);

    void on_message(peer_id_t source_peer, read_stream_t *stream);

    void mailbox_read_coroutine(peer_id_t source_peer, threadnum_t dest_thread,
                                raw_mailbox_t::id_t dest_mailbox_id,
                                std::vector<char> *stream_data,
                                int64_t stream_data_offset);

    template<class... arg_ts>
    void local_delivery_coroutine(threadnum_t dest_thread,
                                  raw_mailbox_t::id_t dest,
                                  const arg_ts&... data);
};


/* Template member implementations */

template<class... arg_ts>
bool mailbox_manager_t::try_local_delivery(const raw_mailbox_t::address_t &dest,
                                           const arg_ts&... data) {
    // Check if dest is a local mailbox
    raw_mailbox_t *potential_dest_mbox =
        mailbox_tables.get()->find_mailbox(dest.mailbox_id);
    if (potential_dest_mbox != NULL) {
        if (potential_dest_mbox->get_address().peer != dest.peer) {
            // Nope, dest is on a different host.
            return false;
        } else {
            // Ok, it's local. Deliver the message.
            threadnum_t dest_thread(
                dest.thread == raw_mailbox_t::address_t::ANY_THREAD
                ? get_thread_id().threadnum
                : dest.thread);
            // This is `spawn_now_dangerously` for performance reasons.
            // It cuts query latency by >20% in some scenarios compared to
            // `spawn_sometime`.
            coro_t::spawn_now_dangerously(
                std::bind(&mailbox_manager_t::local_delivery_coroutine<arg_ts...>,
                          this, dest_thread, dest.mailbox_id, data...));
            return true;
        }
    }
    return false;
}

template<class... arg_ts>
void mailbox_manager_t::local_delivery_coroutine(threadnum_t dest_thread,
                                                 raw_mailbox_t::id_t dest,
                                                 const arg_ts&... data) {
    on_thread_t rethreader(dest_thread);
    if (rethreader.home_thread() == dest_thread) {
        // Some message handlers might not expect messages to be delivered
        // immediately (there could be issues with reentrancy).
        // So we make sure that we yield at least once before delivering the
        // message. Note that we don't yield again if on_thread_t already had
        // to switch the thread (in which case it will already have yielded).
        coro_t::yield();
    }
    // Check if the mailbox still exists (if not: ignore)
    raw_mailbox_t *mbox = mailbox_tables.get()->find_mailbox(dest);
    if (mbox != NULL) {
        const std::function<void(arg_ts...)> *cb =
            static_cast<const std::function<void(arg_ts...)> *>(
                mbox->callback->get_local_delivery_cb());
        guarantee(cb != NULL);
        (*cb)(data...);
    }
}


#endif /* RPC_MAILBOX_MAILBOX_HPP_ */
