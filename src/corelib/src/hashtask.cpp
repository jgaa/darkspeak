
#include <array>

#include "ds/hashtask.h"

#include "logfault/logfault.h"

using namespace std;

namespace ds {
namespace core {

HashTask::HashTask(QObject *owner, const File::ptr_t& file)
 : QObject{owner}, file_{file} {}

void HashTask::run() {
    try {
        QFile file(getPath());
        if (!file.open(QIODevice::ReadOnly)) {
            emit hashed({}, "Failed to open file");
            return;
        }

        crypto_hash_sha256_state state = {};
        crypto_hash_sha256_init(&state);

        std::array<uint8_t, 1024 * 8> buffer;
        while(!file.isOpen()) {
            if (file_->getState() != File::FS_HASHING) {
                LFLOG_WARN << "File #" << file_->getId()
                           << " changed state during hashing. Aborting!";

                emit hashed({}, "Aborted");
                file.close();
                return;
            }

            const auto bytes_read = file.read(reinterpret_cast<char *>(buffer.data()),
                                              static_cast<qint64>(buffer.size()));
            if (bytes_read > 0) {
                crypto_hash_sha256_update(&state, buffer.data(),
                                          static_cast<size_t>(bytes_read));
            } else if (bytes_read == 0) {
                file.close();
            } else {
                emit hashed({}, "Read failed");
                file.close();
                return;
            }
        }

        QByteArray out;
        out.resize(crypto_hash_sha256_BYTES);
        crypto_hash_sha256_final(&state, reinterpret_cast<uint8_t *>(out.data()));
        emit hashed(out, {});
    } catch(const std::exception& ex) {
        qWarning() << "Caught exception from task: " << ex.what();
        emit hashed({}, ex.what());
    }
}

QString HashTask::getPath() const noexcept {

    // Incoming files are verified before they are renamed to their
    // final names.
    if (file_->getDirection() == File::INCOMING) {
        return file_->getDownloadPath();
    }

    return file_->getPath();
}

}}
