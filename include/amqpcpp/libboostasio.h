/**
 *  LibBoostAsio.h
 *
 *  Implementation for the AMQP::TcpHandler for asio. You can use this class
 *  instead of a AMQP::TcpHandler class, just pass the boost asio service to the 
 *  constructor and you're all set.  See tests/libboostasio.cpp for example.
 *
 *  Watch out: this class was not implemented or reviewed by the original author of 
 *  AMQP-CPP. However, we do get a lot of questions and issues from users of this class,
 *  so we cannot guarantee its quality. If you run into such issues too, it might be
 *  better to implement your own handler that interact with boost.
 *
 *
 *  @author Gavin Smith <gavin.smith@coralbay.tv>
 */


/**
 *  Include guard
 */
#pragma once

/**
 *  Dependencies
 */
#include <atomic>
#include <functional>
#include <memory>

#include <asio/io_service.hpp>
#include <asio/strand.hpp>
#include <asio/error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/posix/stream_descriptor.hpp>

#include "amqpcpp/linux_tcp.h"

// C++17 has 'weak_from_this()' support.
#if __cplusplus >= 201701L
#define PTR_FROM_THIS(T) weak_from_this()
#else
#define PTR_FROM_THIS(T) std::weak_ptr<T>(shared_from_this())
#endif

#define operation_canceled operation_aborted

/**
 *  Set up namespace
 */
namespace AMQP {

/**
 *  Class definition
 *  @note Because of a limitation on Windows, this will only work on POSIX based systems - see https://github.com/chriskohlhoff/asio/issues/70
 */
class LibBoostAsioHandler : public virtual TcpHandler
{
protected:

    /**
     *  Helper class that wraps a boost io_service socket monitor.
     */
    class Watcher : public virtual std::enable_shared_from_this<Watcher>
    {
    private:

        /**
         *  The boost asio io_service which is responsible for detecting events.
         *  @var class asio::io_service&
         */
        asio::io_service & _ioservice;

        using strand_weak_ptr = std::weak_ptr<asio::io_service::strand>;

        /**
         *  The boost asio io_service::strand managed pointer.
         *  @var class std::shared_ptr<asio::io_service>
         */
        strand_weak_ptr _wpstrand;

        /**
         *  The boost tcp socket.
         *  @var class asio::ip::tcp::socket
         *  @note https://stackoverflow.com/questions/38906711/destroying-boost-asio-socket-without-closing-native-handler
         */
        asio::posix::stream_descriptor _socket;

        /**
         *  A boolean that indicates if the watcher is monitoring for read events.
         *  @var _read True if reads are being monitored else false.
         */
        bool _read{false};

        /**
         *  A boolean that indicates if the watcher has a pending read event.
         *  @var _read True if read is pending else false.
         */
        std::atomic<bool> _read_pending{false};

        /**
         *  A boolean that indicates if the watcher is monitoring for write events.
         *  @var _read True if writes are being monitored else false.
         */
        bool _write{false};

        /**
         *  A boolean that indicates if the watcher has a pending write event.
         *  @var _read True if read is pending else false.
         */
        std::atomic<bool> _write_pending{false};

        using handler_cb = std::function<void(asio::error_code, std::size_t)>;

        /**
         * Binds and returns a read handler for the io operation.
         * @param  connection   The connection being watched.
         * @param  fd           The file descripter being watched.
         * @return handler callback
         */
        handler_cb get_read_handler(TcpConnection *const connection, const int fd)
        {
            const std::weak_ptr<Watcher> awpWatcher = PTR_FROM_THIS(Watcher);

            return [connection, fd, awpWatcher](const asio::error_code& ec, const std::size_t bytes_transferred)
            {
                const std::shared_ptr<Watcher> aWatcher = awpWatcher.lock();
                if (!aWatcher) { return; }

                const std::shared_ptr<asio::io_service::strand> strand = aWatcher->_wpstrand.lock();
                if (!strand)
                {
                    aWatcher->read_handler(
                        asio::error::make_error_code(asio::error::operation_canceled),
                        std::size_t{0},
                        awpWatcher,
                        connection,
                        fd
                    );
                    return;
                }

                strand->dispatch([&ec, bytes_transferred, awpWatcher, connection, fd]()
                {
                    const std::shared_ptr<Watcher> aWatcher = awpWatcher.lock();
                    if (aWatcher)
                    {
                        aWatcher->read_handler(ec, bytes_transferred, awpWatcher, connection, fd);
                    }
                });
			};
        }

        /**
         * Binds and returns a read handler for the io operation.
         * @param  connection   The connection being watched.
         * @param  fd           The file descripter being watched.
         * @return handler callback
         */
        handler_cb get_write_handler(TcpConnection *const connection, const int fd)
        {
            const std::weak_ptr<Watcher> awpWatcher = PTR_FROM_THIS(Watcher);

            return [connection, fd, awpWatcher](const asio::error_code& ec, const std::size_t bytes_transferred)
            {
                const std::shared_ptr<Watcher> aWatcher = awpWatcher.lock();
                if (!aWatcher) { return; }

                const std::shared_ptr<asio::io_service::strand> strand = aWatcher->_wpstrand.lock();
                if (!strand)
                {
                    aWatcher->write_handler(
                        asio::error::make_error_code(asio::error::operation_canceled),
                        std::size_t{0},
                        awpWatcher,
                        connection,
                        fd
                    );
                    return;
                }

                strand->dispatch([&ec, bytes_transferred, awpWatcher, connection, fd]()
				{
                    const std::shared_ptr<Watcher> aWatcher = awpWatcher.lock();
                    if (aWatcher)
                    {
                        aWatcher->write_handler(ec, bytes_transferred, awpWatcher, connection, fd);
                    }
                });
            };
        }

        /**
         *  Handler method that is called by boost's io_service when the socket pumps a read event.
         *  @param  ec          The status of the callback.
         *  @param  bytes_transferred The number of bytes transferred.
         *  @param  awpWatcher  A weak pointer to this object.
         *  @param  connection  The connection being watched.
         *  @param  fd          The file descriptor being watched.
         *  @note   The handler will get called if a read is cancelled.
         */
        void read_handler(const asio::error_code &ec,
                          const std::size_t bytes_transferred,
                          const std::weak_ptr<Watcher> awpWatcher,
                          TcpConnection *const connection,
                          const int fd)
        {
            // Resolve any potential problems with dangling pointers
            // (remember we are using async).
            const std::shared_ptr<Watcher> apWatcher = awpWatcher.lock();
            if (!apWatcher) { return; }

            _read_pending = false;

            if ((!ec || ec == asio::error::would_block) && _read)
            {
                connection->process(fd, AMQP::readable);

                bool expected = false;
                if (_read_pending.compare_exchange_strong(expected, true))
                {
                    _socket.async_read_some(
                        asio::null_buffers(),
                        get_read_handler(connection, fd));
                }
            }
        }

        /**
         *  Handler method that is called by boost's io_service when the socket pumps a write event.
         *  @param  ec          The status of the callback.
         *  @param  bytes_transferred The number of bytes transferred.
         *  @param  awpWatcher  A weak pointer to this object.
         *  @param  connection  The connection being watched.
         *  @param  fd          The file descriptor being watched.
         *  @note   The handler will get called if a write is cancelled.
         */
        void write_handler(const asio::error_code ec,
                           const std::size_t bytes_transferred,
                           const std::weak_ptr<Watcher> awpWatcher,
                           TcpConnection *const connection,
                           const int fd)
        {
            // Resolve any potential problems with dangling pointers
            // (remember we are using async).
            const std::shared_ptr<Watcher> apWatcher = awpWatcher.lock();
            if (!apWatcher) { return; }

            _write_pending = false;

            if ((!ec || ec == asio::error::would_block) && _write)
            {
                connection->process(fd, AMQP::writable);

                bool expected = false;
                if (_write_pending.compare_exchange_strong(expected, true))
                {
                    _socket.async_write_some(
                        asio::null_buffers(),
                        get_write_handler(connection, fd));
                }
            }
        }

    public:
        /**
         *  Constructor- initialises the watcher and assigns the filedescriptor to 
         *  a boost socket for monitoring.
         *  @param  io_service      The boost io_service
         *  @param  wpstrand        A weak pointer to a io_service::strand instance.
         *  @param  fd              The filedescriptor being watched
         */
        Watcher(asio::io_service &io_service,
                const std::weak_ptr<asio::io_service::strand> strand,
                const int fd) :
            _ioservice(io_service),
            _wpstrand(strand),
            _socket(_ioservice)
        {
            _socket.assign(fd);

            _socket.non_blocking(true);
        }

        /**
         *  Watchers cannot be copied or moved
         *
         *  @param  that    The object to not move or copy
         */
        Watcher(Watcher &&that) = delete;
        Watcher(const Watcher &that) = delete;

        /**
         *  Destructor
         */
        ~Watcher()
        {
            _read = false;
            _write = false;
            _socket.release();
        }

        /**
         *  Change the events for which the filedescriptor is monitored
         *  @param  events
         */
        void events(TcpConnection *connection, int fd, int events)
        {
            // 1. Handle reads?
            _read = ((events & AMQP::readable) != 0);

            // Read requested but no read pending?
            if (_read)
            {
                bool expected = false;
                if (_read_pending.compare_exchange_strong(expected, true))
                {
                    _socket.async_read_some(
                        asio::null_buffers(),
                        get_read_handler(connection, fd));
                }
            }

            // 2. Handle writes?
            _write = ((events & AMQP::writable) != 0);

            // Write requested but no write pending?
            if (_write)
            {
                bool expected = false;
                if (_write_pending.compare_exchange_strong(expected, true))
                {
                    _socket.async_write_some(
                        asio::null_buffers(),
                        get_write_handler(connection, fd));
                }
            }
        }
    };

    /**
     *  Timer class to periodically fire a heartbeat
     */
    class Timer : public std::enable_shared_from_this<Timer>
    {
    private:

        /**
         *  The boost asio io_service which is responsible for detecting events.
         *  @var class asio::io_service&
         */
        asio::io_service & _ioservice;

        using strand_weak_ptr = std::weak_ptr<asio::io_service::strand>;

        /**
         *  The boost asio io_service::strand managed pointer.
         *  @var class std::shared_ptr<asio::io_service>
         */
        strand_weak_ptr _wpstrand;

        /**
         *  The boost asynchronous steady timer.
         *  @var class asio::steady_timer
         */
        asio::steady_timer _timer;

        using handler_fn = std::function<void(const asio::error_code&)>;

        /**
         *  The boost asynchronous steady timer.
         *  @var class asio::steady_timer
         */
        handler_fn get_handler(TcpConnection *const connection, const uint16_t timeout)
        {
            const std::weak_ptr<Timer> awpTimer = PTR_FROM_THIS(Timer);

            return [connection, timeout, awpTimer](const asio::error_code& ec)
            {
                const std::shared_ptr<Timer> aTimer = awpTimer.lock();
                if (!aTimer) { return; }

                const std::shared_ptr<asio::io_service::strand> strand = aTimer->_wpstrand.lock();
                if (!strand)
                {
                    aTimer->timeout(
                        asio::error::make_error_code(asio::error::operation_canceled),
                        awpTimer,
                        connection,
                        timeout
                    );
                    return;
                }
                strand->dispatch([&ec, awpTimer, connection, timeout]()
                {
                    const std::shared_ptr<Timer> aTimer = awpTimer.lock();
                    if (aTimer)
                    {
                        aTimer->timeout(ec, awpTimer, connection, timeout);
                    }
                });
            };
        }

        /**
         *  Callback method that is called by libev when the timer expires
         *  @param  ec          error code returned from loop
         *  @param  loop        The loop in which the event was triggered
         *  @param  connection
         *  @param  timeout
         */
        void timeout(const asio::error_code &ec,
                     std::weak_ptr<Timer> awpThis,
                     TcpConnection *const connection,
                     const uint16_t timeout)
        {
            // Resolve any potential problems with dangling pointers
            // (remember we are using async).
            const std::shared_ptr<Timer> apTimer = awpThis.lock();
            if (!apTimer) { return; }

            if (!ec)
            {
                if (connection)
                {
                    // send the heartbeat
                    connection->heartbeat();
                }

                // Reschedule the timer for the future:
                _timer.expires_at(_timer.expires_at() + std::chrono::seconds(timeout));

                // Posts the timer event
                _timer.async_wait(get_handler(connection, timeout));
            }
        }

        /**
         *  Stop the timer
         */
        void stop()
        {
            // do nothing if it was never set
            _timer.cancel();
        }

    public:
        /**
         *  Constructor
         *  @param  io_service The boost asio io_service.
         *  @param  wpstrand   A weak pointer to a io_service::strand instance.
         */
        Timer(asio::io_service &io_service,
              const std::weak_ptr<asio::io_service::strand> strand) :
            _ioservice(io_service),
            _wpstrand(strand),
            _timer(_ioservice)
        {

        }

        /**
         *  Timers cannot be copied or moved
         *
         *  @param  that    The object to not move or copy
         */
        Timer(Timer &&that) = delete;
        Timer(const Timer &that) = delete;

        /**
         *  Destructor
         */
        ~Timer()
        {
            // stop the timer
            stop();
        }

        /**
         *  Change the expire time
         *  @param  connection
         *  @param  timeout
         */
        void set(TcpConnection *connection, uint16_t timeout)
        {
            // stop timer in case it was already set
            stop();

            _timer.expires_from_now(std::chrono::seconds(timeout));
            _timer.async_wait(get_handler(connection, timeout));
        }
    };

    /**
     *  The boost asio io_service.
     *  @var class asio::io_service&
     */
    asio::io_service & _ioservice;

    /**
     *  The boost asio io_service::strand managed pointer.
     *  @var class std::shared_ptr<asio::io_service>
     */
    std::shared_ptr<asio::io_service::strand> _strand;


    /**
     *  All I/O watchers that are active, indexed by their filedescriptor
     *  @var std::map<int,Watcher>
     */
    std::map<int, std::shared_ptr<Watcher> > _watchers;

    /**
     * The boost asio io_service::deadline_timer managed pointer.
     * @var class std::shared_ptr<Timer>
     */
    std::shared_ptr<Timer> _timer;

    /**
     *  Method that is called by AMQP-CPP to register a filedescriptor for readability or writability
     *  @param  connection  The TCP connection object that is reporting
     *  @param  fd          The filedescriptor to be monitored
     *  @param  flags       Should the object be monitored for readability or writability?
     */
    void monitor(TcpConnection *const connection,
                 const int fd,
                 const int flags) override
    {
        // do we already have this filedescriptor
        auto iter = _watchers.find(fd);

        // was it found?
        if (iter == _watchers.end())
        {
            // we did not yet have this watcher - but that is ok if no filedescriptor was registered
            if (flags == 0){ return; }

            // construct a new pair (watcher/timer), and put it in the map
            const std::shared_ptr<Watcher> apWatcher = 
                std::make_shared<Watcher>(_ioservice, _strand, fd);

            _watchers[fd] = apWatcher;

            // explicitly set the events to monitor
            apWatcher->events(connection, fd, flags);
        }
        else if (flags == 0)
        {
            // the watcher does already exist, but we no longer have to watch this watcher
            _watchers.erase(iter);
        }
        else
        {
            // Change the events on which to act.
            iter->second->events(connection,fd,flags);
        }
    }

protected:
    /**
     *  Method that is called when the heartbeat frequency is negotiated between the server and the client.
     *  @param  connection      The connection that suggested a heartbeat interval
     *  @param  interval        The suggested interval from the server
     *  @return uint16_t        The interval to use
     */
    virtual uint16_t onNegotiate(TcpConnection *connection, uint16_t interval) override
    {
        // skip if no heartbeats are needed
        if (interval == 0) return 0;

        // set the timer
        _timer->set(connection, interval);

        // we agree with the interval
        return interval;
    }

public:

    /**
     *  Handler cannot be default constructed.
     *
     *  @param  that    The object to not move or copy
     */
    LibBoostAsioHandler() = delete;

    /**
     *  Constructor
     *  @param  io_service    The boost io_service to wrap
     */
    explicit LibBoostAsioHandler(asio::io_service &io_service) :
        _ioservice(io_service),
        _strand(std::make_shared<asio::io_service::strand>(_ioservice)),
        _timer(std::make_shared<Timer>(_ioservice,_strand))
    {

    }

    /**
     *  Handler cannot be copied or moved
     *
     *  @param  that    The object to not move or copy
     */
    LibBoostAsioHandler(LibBoostAsioHandler &&that) = delete;
    LibBoostAsioHandler(const LibBoostAsioHandler &that) = delete;

    /**
     *  Returns a reference to the boost io_service object that is being used.
     *  @return The boost io_service object.
     */
    asio::io_service &service()
    {
       return _ioservice;
    }

    /**
     *  Destructor
     */
    ~LibBoostAsioHandler() override = default;

    /**
     * Make sure to stop the heartbeat timer after the connection is closed.
     * Otherwise it will keep the service running forever.
     */
    void onClosed(TcpConnection* connection) override
    {
        (void)connection;
        _timer.reset();
    }
};


/**
 *  End of namespace
 */
}
