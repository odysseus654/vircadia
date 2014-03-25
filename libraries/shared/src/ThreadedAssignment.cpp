//
//  ThreadedAssignment.cpp
//  hifi
//
//  Created by Stephen Birarda on 12/3/2013.
//  Copyright (c) 2013 HighFidelity, Inc. All rights reserved.
//

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonObject>
#include <QtCore/QTimer>

#include "Logging.h"
#include "ThreadedAssignment.h"

ThreadedAssignment::ThreadedAssignment(const QByteArray& packet) :
    Assignment(packet),
    _isFinished(false)
{
    
}

void ThreadedAssignment::deleteLater() {
    // move the NodeList back to the QCoreApplication instance's thread
    NodeList::getInstance()->moveToThread(QCoreApplication::instance()->thread());
    QObject::deleteLater();
}

void ThreadedAssignment::setFinished(bool isFinished) {
    _isFinished = isFinished;

    if (_isFinished) {
        aboutToFinish();
        emit finished();
    }
}

void ThreadedAssignment::commonInit(const QString& targetName, NodeType_t nodeType, bool shouldSendStats) {
    // change the logging target name while the assignment is running
    Logging::setTargetName(targetName);
    
    NodeList* nodeList = NodeList::getInstance();
    nodeList->setOwnerType(nodeType);
    
    QTimer* domainServerTimer = new QTimer(this);
    connect(domainServerTimer, SIGNAL(timeout()), this, SLOT(checkInWithDomainServerOrExit()));
    domainServerTimer->start(DOMAIN_SERVER_CHECK_IN_USECS / 1000);
    
    QTimer* pingNodesTimer = new QTimer(this);
    connect(pingNodesTimer, SIGNAL(timeout()), nodeList, SLOT(pingInactiveNodes()));
    pingNodesTimer->start(PING_INACTIVE_NODE_INTERVAL_USECS / 1000);
    
    QTimer* silentNodeRemovalTimer = new QTimer(this);
    connect(silentNodeRemovalTimer, SIGNAL(timeout()), nodeList, SLOT(removeSilentNodes()));
    silentNodeRemovalTimer->start(NODE_SILENCE_THRESHOLD_USECS / 1000);
    
    if (shouldSendStats) {
        // send a stats packet every 1 second
        QTimer* statsTimer = new QTimer(this);
        connect(statsTimer, &QTimer::timeout, this, &ThreadedAssignment::sendStatsPacket);
        statsTimer->start(1000);
    }
}

void ThreadedAssignment::addPacketStatsAndSendStatsPacket(QJsonObject &statsObject) {
    NodeList* nodeList = NodeList::getInstance();
    
    float packetsPerSecond, bytesPerSecond;
    nodeList->getPacketStats(packetsPerSecond, bytesPerSecond);
    nodeList->resetPacketStats();
    
    statsObject["packets_per_second"] = packetsPerSecond;
    statsObject["bytes_per_second"] = bytesPerSecond;
    
    nodeList->sendStatsToDomainServer(statsObject);
}

void ThreadedAssignment::sendStatsPacket() {
    QJsonObject statsObject;
    addPacketStatsAndSendStatsPacket(statsObject);
}

void ThreadedAssignment::checkInWithDomainServerOrExit() {
    if (NodeList::getInstance()->getNumNoReplyDomainCheckIns() == MAX_SILENT_DOMAIN_SERVER_CHECK_INS) {
        setFinished(true);
    } else {
        qDebug() << "Sending DS check in. There are" << NodeList::getInstance()->getNumNoReplyDomainCheckIns() << "unreplied.";
        NodeList::getInstance()->sendDomainServerCheckIn();
    }
}

bool ThreadedAssignment::readAvailableDatagram(QByteArray& destinationByteArray, HifiSockAddr& senderSockAddr) {
    NodeList* nodeList = NodeList::getInstance();
    
    if (nodeList->getNodeSocket().hasPendingDatagrams()) {
        destinationByteArray.resize(nodeList->getNodeSocket().pendingDatagramSize());
        nodeList->getNodeSocket().readDatagram(destinationByteArray.data(), destinationByteArray.size(),
                                               senderSockAddr.getAddressPointer(), senderSockAddr.getPortPointer());
        return true;
    } else {
        return false;
    }
}
