/*
This file is part of FlashMQ (https://www.flashmq.org)
Copyright (C) 2021 Wiebe Cazemier

FlashMQ is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as
published by the Free Software Foundation, version 3.

FlashMQ is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public
License along with FlashMQ. If not, see <https://www.gnu.org/licenses/>.
*/

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <exception>
#include <stdexcept>
#include <stdio.h>
#include <cstring>

#include "retainedmessagesdb.h"
#include "utils.h"
#include "logger.h"
#include "mqttpacket.h"
#include "threadglobals.h"
#include "client.h"
#include "logger.h"

RetainedMessagesDB::RetainedMessagesDB(const std::string &filePath) : PersistenceFile(filePath)
{

}

void RetainedMessagesDB::openWrite()
{
    PersistenceFile::openWrite(MAGIC_STRING_V4);
}

void RetainedMessagesDB::openRead()
{
    PersistenceFile::openRead();

    if (detectedVersionString == MAGIC_STRING_V1)
        readVersion = ReadVersion::v1;
    else if (detectedVersionString == MAGIC_STRING_V2)
        readVersion = ReadVersion::v2;
    else if (detectedVersionString == MAGIC_STRING_V3)
        readVersion = ReadVersion::v3;
    else if (detectedVersionString == MAGIC_STRING_V4)
        readVersion = ReadVersion::v4;
    else
        throw std::runtime_error("Unknown file version.");
}

/**
 * @brief RetainedMessagesDB::saveData doesn't explicitely name a file version (v1, etc), because we always write the current definition.
 * @param messages
 */
void RetainedMessagesDB::saveData(const std::vector<RetainedMessage> &messages)
{
    if (!f)
        return;

    CirBuf cirbuf(1024);

    const int64_t now_epoch = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    logger->logf(LOG_DEBUG, "Saving current time stamp %ld in retained messages DB.", now_epoch);
    writeInt64(now_epoch);

    writeUint32(messages.size());

    char reserved[RESERVED_SPACE_RETAINED_DB_V2];
    std::memset(reserved, 0, RESERVED_SPACE_RETAINED_DB_V2);
    writeCheck(reserved, 1, RESERVED_SPACE_RETAINED_DB_V2, f);

    for (const RetainedMessage &rm : messages)
    {
        logger->logf(LOG_DEBUG, "Saving retained message for topic '%s' QoS %d, age %d seconds.", rm.publish.topic.c_str(), rm.publish.qos, rm.publish.getAge());

        Publish pcopy(rm.publish);
        MqttPacket pack(ProtocolVersion::Mqtt5, pcopy);

        // Dummy, to please the parser on reading.
        if (pcopy.qos > 0)
            pack.setPacketId(666);

        const uint32_t packSize = pack.getSizeIncludingNonPresentHeader();
        const uint32_t pubAge = ageFromTimePoint(pcopy.getCreatedAt());

        cirbuf.reset();
        cirbuf.ensureFreeSpace(packSize + 32);
        pack.readIntoBuf(cirbuf);

        writeUint16(pack.getFixedHeaderLength());
        writeUint32(pubAge);
        writeUint32(packSize);
        writeString(pcopy.client_id);
        writeString(pcopy.username);
        writeCheck(cirbuf.tailPtr(), 1, cirbuf.usedBytes(), f);
    }

    fflush(f);
}

std::list<RetainedMessage> RetainedMessagesDB::readData()
{
    std::list<RetainedMessage> defaultResult;

    if (!f)
        return defaultResult;

    if (readVersion == ReadVersion::v1)
        logger->logf(LOG_WARNING, "File '%s' is version 1, an internal development version that was never finalized. Not reading.", getFilePath().c_str());
    if (readVersion == ReadVersion::v2)
        logger->logf(LOG_WARNING, "File '%s' is version 2, an internal development version that was never finalized. Not reading.", getFilePath().c_str());
    if (readVersion == ReadVersion::v3 || readVersion == ReadVersion::v4)
        return readDataV3V4();

    return defaultResult;
}

std::list<RetainedMessage> RetainedMessagesDB::readDataV3V4()
{
    std::list<RetainedMessage> messages;

    CirBuf cirbuf(1024);

    const Settings &settings = *ThreadGlobals::getSettings();
    std::shared_ptr<ThreadData> dummyThreadData;
    std::shared_ptr<Client> dummyClient(new Client(0, dummyThreadData, nullptr, false, false, nullptr, settings, false));
    dummyClient->setClientProperties(ProtocolVersion::Mqtt5, "Dummyforloadingretained", "nobody", true, 60);

    while (!feof(f))
    {
        bool eofFound = false;

        int64_t persistence_state_age = 0;

        if (readVersion >= ReadVersion::v4)
        {
            const int64_t fileSavedAt = readInt64(eofFound);
            if (eofFound)
                continue;

            const int64_t now_epoch = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            persistence_state_age = fileSavedAt > now_epoch ? 0 : now_epoch - fileSavedAt;
        }

        const uint32_t numberOfMessages = readUint32(eofFound);

        if (eofFound)
            continue;

        fseek(f, RESERVED_SPACE_RETAINED_DB_V2, SEEK_CUR);

        for(uint32_t i = 0; i < numberOfMessages; i++)
        {
            const uint16_t fixed_header_length = readUint16(eofFound);
            uint32_t originalPubAge = 0;
            if (readVersion >= ReadVersion::v4)
            {
                originalPubAge = readUint32(eofFound);
            }
            const uint32_t newPubAge = persistence_state_age + originalPubAge;
            const uint32_t packlen = readUint32(eofFound);

            const std::string client_id = readString(eofFound);
            const std::string username = readString(eofFound);

            if (eofFound)
                continue;

            cirbuf.reset();
            cirbuf.ensureFreeSpace(packlen + 32);

            readCheck(cirbuf.headPtr(), 1, packlen, f);
            cirbuf.advanceHead(packlen);
            MqttPacket pack(cirbuf, packlen, fixed_header_length, dummyClient);

            pack.parsePublishData();
            Publish pub(pack.getPublishData());

            pub.client_id = client_id;
            pub.username = username;

            // A createdAt only means something when there is expire info (internal boolean is true), so we fake it first.
            pub.setExpireAfter(std::numeric_limits<uint32_t>::max());
            pub.createdAt = timepointFromAge(newPubAge);

            RetainedMessage msg(pub);
            logger->logf(LOG_DEBUG, "Loading retained message for topic '%s' QoS %d, age %d seconds.", msg.publish.topic.c_str(), msg.publish.qos, msg.publish.getAge());
            messages.push_back(std::move(msg));
        }
    }

    return messages;
}
