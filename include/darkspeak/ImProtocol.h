#pragma once

#include <string>
#include <memory>
#include <boost/asio.hpp>
#include <boost/asio/yield.hpp>
#include <boost/filesystem.hpp>

#include "darkspeak/Api.h"

namespace darkspeak {

/*! \class ImProtocol ImProtocol.h darkspeak/ImProtocol.h
 *
 * General interface to an abstract IM protocol.
 *
 * This is targeted to the legacy Tor Chat protocol,
 * but we may very well switch the implementation to another
 * protocol for darkspeak clients in the future to allow
 * better security, performance or other goals. However
 * we want to remain compatible with Tor Chat.
 *
 * So this interface must work optimally with TC, without
 * putting restraints on future IM protocols.
 *
 * There should be only one instance of one particular implementation
 * of this interface at any time.
 */
class ImProtocol
{
public:
    /// Message
    struct Message {
        std::string text;
    };

    struct File {
        path_t path;
    };

    struct FileError {
        std::string error_description;
    };

    /*! Interface to monitor a file transfer.
     *
     */
    class FileMonitor {
    public:
        using ptr_t = std::shared_ptr<FileMonitor>;

        /// Called when the transfer is successfully completed
        virtual void OnSuccessfullyDone() = 0;

        /*! Called if the transfer failed, was canceled or otherwise
         * did not succeed.
         */
        virtual void OnError(const FileError& error) = 0;

        /// Called during transfer when data is sent or received
        virtual void Progress(uint percent) = 0;

        /// Get the transfer token for the file
        virtual std::string GetFileId() = 0;
    };

    /*! Interface to get incoming events
     *
     */
    class EventMonitor {
    public:
        using ptr_t =  std::shared_ptr<EventMonitor>;

        /*! Incoming connection from someone
         *
         * \return True if you want to proceed with the connection
         */
        virtual bool OnIncomingConnecction() = 0;

        /*! Someone requests to be added as a buddy
         *
         * \return true if the buddy was added
         */
        virtual bool OnAddNewBuddy() = 0;

        /*! A buddy's state was changed
         *
         */
        virtual void OnBuddyStateChange() = 0;

        /*! An incoming message has arrived
         */
        virtual void OnIncomingMessafe() = 0;

        /*! Someone requests to send us a file
         *
         * \return FileMonitor pointer if we want to receive the file
         *      else nullptr.
         */
        virtual FileMonitor::ptr_t OnIncomingFile() = 0;

        /*! Some other event happened that the user may want to know about.
         */
        virtual void OnOtherEvent() = 0;
    };

    virtual ~ImProtocol() = default;

    /*! Connect to a peer.
     *
     * When Connect returns, we are connected.
     *
     * \exception WarError derived exceptions on errors.
     */
    virtual void Connect(Buddy::ptr_t& buddy,
                         boost::asio::yield_context& yield) = 0;


    virtual void SendMessage(Buddy& buddy, const Message& msg,
                             boost::asio::yield_context& yield) = 0;

    /*! Starts a file transfer.
     *
     * The function returns when the transfer is set up to send and receive data.
     *
     * \param file Information about the file to send
     * \param monitor Information sharing from the transfer to the
     *          initiator.
     */
    virtual void SendFile(Buddy& buddy, const File& file,
        FileMonitor::ptr_t monitor,
        boost::asio::yield_context& yield) = 0;

    /*! Set the event monitor to receive events from this instance
     *
     */
    virtual void SetMonitor(EventMonitor::ptr_t monitor) = 0;

    /*! Start listening for incoming connections */
    virtual void Listen() = 0;
};


} // namespace
