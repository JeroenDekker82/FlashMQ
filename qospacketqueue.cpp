#include "qospacketqueue.h"

#include "cassert"

#include "mqttpacket.h"

QueuedPublish::QueuedPublish(Publish &&publish, uint16_t packet_id) :
    publish(std::move(publish)),
    packet_id(packet_id)
{

}

uint16_t QueuedPublish::getPacketId() const
{
    return this->packet_id;
}

const Publish &QueuedPublish::getPublish() const
{
    return publish;
}

size_t QueuedPublish::getApproximateMemoryFootprint() const
{
    return publish.topic.length() + publish.payload.length();
}


void QoSPublishQueue::erase(const uint16_t packet_id)
{
    auto it = queue.begin();
    auto end = queue.end();
    while (it != end)
    {
        QueuedPublish &p = *it;
        if (p.getPacketId() == packet_id)
        {
            size_t mem = p.getApproximateMemoryFootprint();
            qosQueueBytes -= mem;
            assert(qosQueueBytes >= 0);
            if (qosQueueBytes < 0) // Should not happen, but correcting a hypothetical bug is fine for this purpose.
                qosQueueBytes = 0;

            queue.erase(it);

            break;
        }

        it++;
    }
}

std::list<QueuedPublish>::const_iterator QoSPublishQueue::erase(std::list<QueuedPublish>::const_iterator pos)
{
    return this->queue.erase(pos);
}

size_t QoSPublishQueue::size() const
{
    return queue.size();
}

size_t QoSPublishQueue::getByteSize() const
{
    return qosQueueBytes;
}

void QoSPublishQueue::queuePublish(PublishCopyFactory &copyFactory, uint16_t id, char new_max_qos)
{
    assert(new_max_qos > 0);
    assert(id > 0);

    Publish pub = copyFactory.getNewPublish();
    pub.splitTopic = false;
    queue.emplace_back(std::move(pub), id);
    qosQueueBytes += queue.back().getApproximateMemoryFootprint();
}

void QoSPublishQueue::queuePublish(Publish &&pub, uint16_t id)
{
    assert(id > 0);

    pub.splitTopic = false;
    queue.emplace_back(std::move(pub), id);
    qosQueueBytes += queue.back().getApproximateMemoryFootprint();
}

std::list<QueuedPublish>::const_iterator QoSPublishQueue::begin() const
{
    return queue.begin();
}

std::list<QueuedPublish>::const_iterator QoSPublishQueue::end() const
{
    return queue.end();
}
