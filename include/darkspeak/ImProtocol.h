#pragma once

#include <string>
#include <memory>
#include <functional>

#include "tasks/WarPipeline.h"

#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <boost/property_tree/ptree.hpp>

#include "darkspeak/Api.h"


namespace darkspeak {

class EventMonitor;
class Config;

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
 */
class ImProtocol
{
public:
    using ptr_t = std::shared_ptr<ImProtocol>;
    using get_pipeline_fn_t = std::function<war::Pipeline&()>;

    struct File {
        path_t path;
    };

    struct FileError {
        std::string error_description;
    };

    struct Stat {
        uint64_t num_incoming_connections = 0;
        uint64_t num_outgoing_connections = 0;
        uint64_t packets_sent = 0;
        uint64_t packets_received = 0;
        uint64_t messages_sent = 0;
        uint64_t messages_received = 0;
        uint64_t bytes_sent = 0;
        uint64_t bytes_received = 0;
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
        virtual void Progress(unsigned int percent) = 0;

        /// Get the transfer token for the file
        virtual std::string GetFileId() = 0;
    };

    virtual ~ImProtocol() = default;

    /*! Connect to a peer.
     *
     * Start to asynchrouneosly connect to a peer.
     *
     * The result will be signalled trough the OnBuddyStateChange
     * signal handler. If the state is connected the call succeeded,
     * if it is offline, it failed.
     *
     * \exception WarError derived exceptions on errors.
     * \exception std::error derived exceptions if hostname/port
     *          configuration is wrong.
     */
    virtual void Connect(Api::Buddy::ptr_t buddy) = 0;

    virtual void SendMessage(Api::Buddy& buddy,
                             const Api::Message::ptr_t& msg) = 0;

    /*! Starts a file transfer.
     *
     * The function returns when the transfer is set up to send and receive data.
     *
     * \param file Information about the file to send
     * \param monitor Information sharing from the transfer to the
     *          initiator.
     */
    virtual void SendFile(Api::Buddy& buddy, const File& file,
        FileMonitor::ptr_t monitor) = 0;

    /*! Accept a file transfer
     */
    virtual void AcceptFileTransfer(const AcceptFileTransferData& aftd) = 0;

    /*! Reject a file transfer
     */
    virtual void RejectFileTransfer(const AcceptFileTransferData& aftd) = 0;

    /*! Abort a file transfer. */
    virtual void AbortFileTransfer(const AbortFileTransferData& aftd) = 0;

    /*! Disconnect a buddy */
    virtual void Disconnect(Api::Buddy& buddy) = 0;

    /*! Set the event monitor to receive events from this instance
     *
     */
    virtual void SetMonitor(std::shared_ptr<EventMonitor> monitor) = 0;

    /*! Start listening for incoming connections */
    virtual void Listen(boost::asio::ip::tcp::endpoint endpoint) = 0;

    /*! Close all connections, stop listening */
    virtual void Shutdown() = 0;

    /*! Create an instance of the implemnetation of the protocol */
    static ptr_t CreateProtocol(get_pipeline_fn_t fn,
                                Config& properties);
};


} // namespace
