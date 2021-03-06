// message_server_port.cpp

/*    Copyright 2009 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/pch.h"

#include <boost/thread/thread.hpp>

#ifndef USE_ASIO


#include "mongo/db/lasterror.h"
#include "mongo/db/server_options.h"
#include "mongo/db/stats/counters.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/net/listen.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/message_port.h"
#include "mongo/util/net/message_server.h"
#include "mongo/util/net/ssl_manager.h"

#ifdef __linux__  // TODO: consider making this ifndef _WIN32
# include <sys/resource.h>
#endif

#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

namespace mongo {

    class PortMessageServer : public MessageServer , public Listener {
    public:
        /**
         * Creates a new message server.
         *
         * @param opts
         * @param handler the handler to use. Caller is responsible for managing this object
         *     and should make sure that it lives longer than this server.
         */
        PortMessageServer(  const MessageServer::Options& opts, MessageHandler * handler ) :
            Listener( "" , opts.ipList, opts.port ), _handler(handler) {
        }

        virtual void acceptedMP(MessagingPort * p) {

            if ( ! Listener::globalTicketHolder.tryAcquire() ) {
                log() << "connection refused because too many open connections: " << Listener::globalTicketHolder.used() << endl;

                // TODO: would be nice if we notified them...
                p->shutdown();
                delete p;

                sleepmillis(2); // otherwise we'll hard loop
                return;
            }

            try {
#ifndef __linux__  // TODO: consider making this ifdef _WIN32
                {
                    HandleIncomingMsgParam* himParam = new HandleIncomingMsgParam(p, _handler);
                    boost::thread thr(stdx::bind(&handleIncomingMsg, himParam));
                }
#else
                pthread_attr_t attrs;
                pthread_attr_init(&attrs);
                pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_DETACHED);

                static const size_t STACK_SIZE = 1024*1024; // if we change this we need to update the warning

                struct rlimit limits;
                verify(getrlimit(RLIMIT_STACK, &limits) == 0);
                if (limits.rlim_cur > STACK_SIZE) {
                    size_t stackSizeToSet = STACK_SIZE;
#if !__has_feature(address_sanitizer)
                    if (DEBUG_BUILD)
                        stackSizeToSet /= 2;
#endif
                    pthread_attr_setstacksize(&attrs, stackSizeToSet);
                } else if (limits.rlim_cur < 1024*1024) {
                    warning() << "Stack size set to " << (limits.rlim_cur/1024) << "KB. We suggest 1MB" << endl;
                }


                pthread_t thread;
                HandleIncomingMsgParam* himParam = new HandleIncomingMsgParam(p, _handler);
                int failed = pthread_create(&thread, &attrs, &handleIncomingMsg, himParam);

                pthread_attr_destroy(&attrs);

                if (failed) {
                    log() << "pthread_create failed: " << errnoWithDescription(failed) << endl;
                    throw boost::thread_resource_error(); // for consistency with boost::thread
                }
#endif
            }
            catch ( boost::thread_resource_error& ) {
                Listener::globalTicketHolder.release();
                log() << "can't create new thread, closing connection" << endl;

                p->shutdown();
                delete p;

                sleepmillis(2);
            }
            catch ( ... ) {
                Listener::globalTicketHolder.release();
                log() << "unknown error accepting new socket" << endl;

                p->shutdown();
                delete p;

                sleepmillis(2);
            }

        }

        virtual void setAsTimeTracker() {
            Listener::setAsTimeTracker();
        }

        virtual void setupSockets() {
            Listener::setupSockets();
        }

        void run() {
            initAndListen();
        }

        virtual bool useUnixSockets() const { return true; }

    private:
        MessageHandler* _handler;

        /**
         * Simple holder for threadRun parameters. Should not destroy the objects it holds -
         * it is the responsibility of the caller to take care of them.
         */
        struct HandleIncomingMsgParam {
            HandleIncomingMsgParam(MessagingPort* inPort,  MessageHandler* handler):
                inPort(inPort), handler(handler) {
            }

            MessagingPort* inPort;
            MessageHandler* handler;
        };

        /**
         * Handles incoming messages from a given socket.
         *
         * Terminating conditions:
         * 1. Assertions while handling the request.
         * 2. Socket is closed.
         * 3. Server is shutting down (based on inShutdown)
         *
         * @param arg this method is in charge of cleaning up the arg object.
         *
         * @return NULL
         */
        static void* handleIncomingMsg(void* arg) {
            TicketHolderReleaser connTicketReleaser( &Listener::globalTicketHolder );

            scoped_ptr<HandleIncomingMsgParam> himArg(static_cast<HandleIncomingMsgParam*>(arg));
            MessagingPort* inPort = himArg->inPort;
            MessageHandler* handler = himArg->handler;

            {
                string threadName = "conn";
                if ( inPort->connectionId() > 0 )
                    threadName = str::stream() << threadName << inPort->connectionId();
                setThreadName( threadName.c_str() );
            }

            verify( inPort );
            inPort->psock->setLogLevel(logger::LogSeverity::Debug(1));
            scoped_ptr<MessagingPort> p( inPort );

            string otherSide;

            Message m;
            try {
                LastError * le = new LastError();
                lastError.reset( le ); // lastError now has ownership

                otherSide = p->psock->remoteString();

                handler->connected( p.get() );

                while ( ! inShutdown() ) {
                    m.reset();
                    p->psock->clearCounters();

                    if ( ! p->recv(m) ) {
                        if (!serverGlobalParams.quiet) {
                            int conns = Listener::globalTicketHolder.used()-1;
                            const char* word = (conns == 1 ? " connection" : " connections");
                            log() << "end connection " << otherSide << " (" << conns << word << " now open)" << endl;
                        }
                        p->shutdown();
                        break;
                    }

                    handler->process( m , p.get() , le );
                    networkCounter.hit( p->psock->getBytesIn() , p->psock->getBytesOut() );
                }
            }
            catch ( AssertionException& e ) {
                log() << "AssertionException handling request, closing client connection: " << e << endl;
                p->shutdown();
            }
            catch ( SocketException& e ) {
                log() << "SocketException handling request, closing client connection: " << e << endl;
                p->shutdown();
            }
            catch ( const DBException& e ) { // must be right above std::exception to avoid catching subclasses
                log() << "DBException handling request, closing client connection: " << e << endl;
                p->shutdown();
            }
            catch ( std::exception &e ) {
                error() << "Uncaught std::exception: " << e.what() << ", terminating" << endl;
                dbexit( EXIT_UNCAUGHT );
            }

            // Normal disconnect path.
#ifdef MONGO_SSL
            SSLManagerInterface* manager = getSSLManager();
            if (manager)
                manager->cleanupThreadLocals();
#endif
            handler->disconnected( p.get() );

            return NULL;
        }
    };


    MessageServer * createServer( const MessageServer::Options& opts , MessageHandler * handler ) {
        return new PortMessageServer( opts , handler );
    }

}

#endif
