﻿#include "ServerSession.h"

#include <list>
#include <map>

#include "RtspSession/StatusCode.h"

#include "Log.h"


namespace {

struct MediaSession
{
    bool recorder = false;
    std::string uri;
    std::unique_ptr<rtsp::Request> createRequest; // describe or announce
    std::unique_ptr<WebRTCPeer> localPeer;
};

typedef std::map<rtsp::SessionId, std::unique_ptr<MediaSession>> MediaSessions;

struct RequestInfo
{
    std::unique_ptr<rtsp::Request> requestPtr;
    const rtsp::SessionId session;
};

typedef std::map<rtsp::CSeq, RequestInfo> Requests;

const auto Log = ServerSessionLog;

}

struct ServerSession::Private
{
    struct AutoEraseRequest;
    struct AutoEraseRecordRequest;

    Private(
        ServerSession* owner,
        std::function<std::unique_ptr<WebRTCPeer> (const std::string& uri)> createPeer);
    Private(
        ServerSession* owner,
        std::function<std::unique_ptr<WebRTCPeer> (const std::string& uri)> createPeer,
        std::function<std::unique_ptr<WebRTCPeer> (const std::string& uri)> createRecordPeer);

    ServerSession* owner;
    std::function<std::unique_ptr<WebRTCPeer> (const std::string& uri)> createPeer;
    std::function<std::unique_ptr<WebRTCPeer> (const std::string& uri)> createRecordPeer;

    std::deque<std::string> iceServers;

    Requests describeRequests;
    Requests announceRequests;
    MediaSessions mediaSessions;

    bool recordEnabled()
        { return createRecordPeer ? true : false; }

    std::string nextSession()
        { return std::to_string(_nextSession++); }

    void streamerPrepared(rtsp::CSeq describeRequestCSeq);
    void recorderPrepared(rtsp::CSeq announceRequestCSeq);
    void iceCandidate(
        const rtsp::SessionId&,
        unsigned, const std::string&);
    void eos(const rtsp::SessionId& session);

private:
    unsigned _nextSession = 1;
};

struct ServerSession::Private::AutoEraseRequest
{
    AutoEraseRequest(
        ServerSession::Private* owner,
        Requests::const_iterator it) :
        _owner(owner), _it(it) {}
    ~AutoEraseRequest()
        { if(_owner) _owner->describeRequests.erase(_it); }
    void discard()
        { _owner = nullptr; }

private:
    ServerSession::Private* _owner;
    Requests::const_iterator _it;
};

struct ServerSession::Private::AutoEraseRecordRequest
{
    AutoEraseRecordRequest(
        ServerSession::Private* owner,
        Requests::const_iterator it) :
        _owner(owner), _it(it) {}
    ~AutoEraseRecordRequest()
        { if(_owner) _owner->announceRequests.erase(_it); }
    void discard()
        { _owner = nullptr; }

private:
    ServerSession::Private* _owner;
    Requests::const_iterator _it;
};

ServerSession::Private::Private(
    ServerSession* owner,
    std::function<std::unique_ptr<WebRTCPeer> (const std::string& uri)> createPeer) :
    owner(owner), createPeer(createPeer)
{
}

ServerSession::Private::Private(
    ServerSession* owner,
    std::function<std::unique_ptr<WebRTCPeer> (const std::string& uri)> createPeer,
    std::function<std::unique_ptr<WebRTCPeer> (const std::string& uri)> createRecordPeer) :
    owner(owner), createPeer(createPeer), createRecordPeer(createRecordPeer)
{
}

void ServerSession::Private::streamerPrepared(rtsp::CSeq describeRequestCSeq)
{
    auto requestIt = describeRequests.find(describeRequestCSeq);
    if(describeRequests.end() == requestIt ||
       rtsp::Method::DESCRIBE != requestIt->second.requestPtr->method)
    {
        owner->disconnect();
        return;
    }

    AutoEraseRequest autoEraseRequest(this, requestIt);

    RequestInfo& requestInfo = requestIt->second;
    const rtsp::SessionId& session = requestInfo.session;

    auto it = mediaSessions.find(session);
    if(mediaSessions.end() == it || it->second->recorder) {
        owner->disconnect();
        return;
    }

    MediaSession& mediaSession = *(it->second);
    WebRTCPeer& localPeer = *mediaSession.localPeer;

    if(localPeer.sdp().empty())
        owner->disconnect();
    else {
        rtsp::Response response;
        prepareOkResponse(requestInfo.requestPtr->cseq, session, &response);

        response.headerFields.emplace("Content-Type", "application/sdp");

        response.body = localPeer.sdp();

        owner->sendResponse(response);

        mediaSession.createRequest.swap(requestInfo.requestPtr);
    }
}

void ServerSession::Private::recorderPrepared(rtsp::CSeq announceRequestCSeq)
{
    auto requestIt = announceRequests.find(announceRequestCSeq);
    if(announceRequests.end() == requestIt ||
       rtsp::Method::ANNOUNCE != requestIt->second.requestPtr->method)
    {
        owner->disconnect();
        return;
    }

    AutoEraseRecordRequest autoEraseRequest(this, requestIt);

    RequestInfo& requestInfo = requestIt->second;
    const rtsp::SessionId& session = requestInfo.session;

    auto it = mediaSessions.find(session);
    if(mediaSessions.end() == it || !it->second->recorder) {
        owner->disconnect();
        return;
    }

    MediaSession& mediaSession = *(it->second);
    WebRTCPeer& recorder = *mediaSession.localPeer;

    if(recorder.sdp().empty())
        owner->disconnect();
    else {
        rtsp::Response response;
        prepareOkResponse(requestInfo.requestPtr->cseq, session, &response);

        response.headerFields.emplace("Content-Type", "application/sdp");

        response.body = recorder.sdp();

        owner->sendResponse(response);

        mediaSession.createRequest.swap(requestInfo.requestPtr);
    }
}

void ServerSession::Private::iceCandidate(
    const rtsp::SessionId& session,
    unsigned mlineIndex, const std::string& candidate)
{
    auto it = mediaSessions.find(session);
    if(mediaSessions.end() == it) {
        owner->disconnect();
        return;
    }

    const MediaSession& mediaSession = *(it->second);

    owner->requestSetup(
        mediaSession.uri,
        "application/x-ice-candidate",
        session,
        std::to_string(mlineIndex) + "/" + candidate + "\r\n");
}

void ServerSession::Private::eos(const rtsp::SessionId& session)
{
    Log()->trace("Eos. Session: {}", session);

    owner->onEos();
}


ServerSession::ServerSession(
    const std::function<std::unique_ptr<WebRTCPeer> (const std::string& uri)>& createPeer,
    const std::function<void (const rtsp::Request*)>& sendRequest,
    const std::function<void (const rtsp::Response*)>& sendResponse) noexcept :
    rtsp::ServerSession(sendRequest, sendResponse),
    _p(new Private(this, createPeer))
{
}

ServerSession::ServerSession(
    const std::function<std::unique_ptr<WebRTCPeer> (const std::string& uri)>& createPeer,
    const std::function<std::unique_ptr<WebRTCPeer> (const std::string& uri)>& createRecordPeer,
    const std::function<void (const rtsp::Request*)>& sendRequest,
    const std::function<void (const rtsp::Response*)>& sendResponse) noexcept :
    rtsp::ServerSession(sendRequest, sendResponse),
    _p(new Private(this, createPeer, createRecordPeer))
{
}

ServerSession::~ServerSession()
{
}

void ServerSession::setIceServers(const WebRTCPeer::IceServers& iceServers)
{
    _p->iceServers = iceServers;
}

bool ServerSession::onOptionsRequest(
    std::unique_ptr<rtsp::Request>& requestPtr) noexcept
{
    rtsp::Response response;
    prepareOkResponse(requestPtr->cseq, rtsp::SessionId(), &response);

    response.headerFields.emplace(
        "Public",
        _p->recordEnabled() ?
            "DESCRIBE, ANNOUNCE, SETUP, PLAY, RECORD, TEARDOWN" :
            "DESCRIBE, SETUP, PLAY, TEARDOWN");

    sendResponse(response);

    return true;
}

bool ServerSession::onDescribeRequest(
    std::unique_ptr<rtsp::Request>& requestPtr) noexcept
{
    std::unique_ptr<WebRTCPeer> peerPtr = _p->createPeer(requestPtr->uri);
    if(!peerPtr)
        return false;

    const rtsp::SessionId session = _p->nextSession();
    auto requestPair =
        _p->describeRequests.emplace(
            requestPtr->cseq,
            RequestInfo {
                .requestPtr = nullptr,
                .session = session,
            });
    if(!requestPair.second)
        return false;

    RequestInfo& requestInfo = requestPair.first->second;
    requestInfo.requestPtr = std::move(requestPtr);
    rtsp::Request& request = *requestInfo.requestPtr;

    Private::AutoEraseRequest autoEraseRequest(_p.get(), requestPair.first);

    auto emplacePair =
        _p->mediaSessions.emplace(
            session,
            std::make_unique<MediaSession>());
    if(!emplacePair.second)
        return false;

    MediaSession& mediaSession = *(emplacePair.first->second);
    mediaSession.recorder = false;
    mediaSession.uri = request.uri;
    mediaSession.localPeer = std::move(peerPtr);

    mediaSession.localPeer->prepare(
        _p->iceServers,
        std::bind(
            &ServerSession::Private::streamerPrepared,
            _p.get(),
            request.cseq),
        std::bind(
            &ServerSession::Private::iceCandidate,
            _p.get(),
            session,
            std::placeholders::_1,
            std::placeholders::_2),
        std::bind(
            &ServerSession::Private::eos,
            _p.get(),
            session));

    autoEraseRequest.discard();

    return true;
}

bool ServerSession::onAnnounceRequest(
    std::unique_ptr<rtsp::Request>& requestPtr) noexcept
{
    if(!_p->recordEnabled())
        return false;

    std::unique_ptr<WebRTCPeer> peerPtr = _p->createRecordPeer(requestPtr->uri);
    if(!peerPtr)
        return false;

    const std::string contentType = RequestContentType(*requestPtr);
    if(contentType != "application/sdp")
        return false;

    const rtsp::SessionId session = _p->nextSession();
    auto requestPair =
        _p->announceRequests.emplace(
            requestPtr->cseq,
            RequestInfo {
                .requestPtr = nullptr,
                .session = session,
            });
    if(!requestPair.second)
        return false;

    RequestInfo& requestInfo = requestPair.first->second;
    requestInfo.requestPtr = std::move(requestPtr);
    rtsp::Request& request = *requestInfo.requestPtr;

    Private::AutoEraseRecordRequest autoEraseRequest(_p.get(), requestPair.first);

    auto emplacePair =
        _p->mediaSessions.emplace(
            session,
            std::make_unique<MediaSession>());
    if(!emplacePair.second)
        return false;

    MediaSession& mediaSession = *(emplacePair.first->second);
    mediaSession.recorder = true;
    mediaSession.uri = request.uri;
    mediaSession.localPeer = std::move(peerPtr);

    mediaSession.localPeer->prepare(
        _p->iceServers,
        std::bind(
            &ServerSession::Private::recorderPrepared,
            _p.get(),
            request.cseq),
        std::bind(
            &ServerSession::Private::iceCandidate,
            _p.get(),
            session,
            std::placeholders::_1,
            std::placeholders::_2),
        std::bind(
            &ServerSession::Private::eos,
            _p.get(),
            session));

    const std::string& sdp = requestInfo.requestPtr->body;
    if(sdp.empty())
        return false;

    mediaSession.localPeer->setRemoteSdp(sdp);

    autoEraseRequest.discard();

    return true;
}

bool ServerSession::onSetupRequest(
    std::unique_ptr<rtsp::Request>& requestPtr) noexcept
{
    const rtsp::SessionId session = RequestSession(*requestPtr);

    auto it = _p->mediaSessions.find(session);
    if(it == _p->mediaSessions.end())
        return false;

    WebRTCPeer& localPeer = *it->second->localPeer;

    if(RequestContentType(*requestPtr) == "application/sdp") {
        localPeer.setRemoteSdp(requestPtr->body);

        sendOkResponse(requestPtr->cseq, session);

        return true;
    }

    if(RequestContentType(*requestPtr) != "application/x-ice-candidate")
        return false;

    const std::string& ice = requestPtr->body;

    std::string::size_type pos = 0;
    while(pos < ice.size()) {
        const std::string::size_type lineEndPos = ice.find("\r\n", pos);
        if(lineEndPos == std::string::npos)
            return false;

        const std::string line = ice.substr(pos, lineEndPos - pos);

        const std::string::size_type delimiterPos = line.find("/");
        if(delimiterPos == std::string::npos || 0 == delimiterPos)
            return false;

        try{
            const int idx = std::stoi(line.substr(0, delimiterPos));
            if(idx < 0)
                return false;

            const std::string candidate =
                line.substr(delimiterPos + 1);

            if(candidate.empty())
                return false;

            Log()->trace("Adding ice candidate \"{}\"", candidate);

            localPeer.addIceCandidate(idx, candidate);
        } catch(...) {
            return false;
        }
        pos = lineEndPos + 2;
    }

    sendOkResponse(requestPtr->cseq, session);

    return true;

}

bool ServerSession::onPlayRequest(
    std::unique_ptr<rtsp::Request>& requestPtr) noexcept
{
    const rtsp::SessionId session = RequestSession(*requestPtr);
    if(session.empty())
        return false;

    auto it = _p->mediaSessions.find(session);
    if(it == _p->mediaSessions.end())
        return false;

    MediaSession& mediaSession = *it->second;
    if(mediaSession.recorder)
        return false;

    WebRTCPeer& localPeer = *(mediaSession.localPeer);

    localPeer.play();

    sendOkResponse(requestPtr->cseq, session);

    return true;
}

bool ServerSession::onRecordRequest(
    std::unique_ptr<rtsp::Request>& requestPtr) noexcept
{
    if(!_p->recordEnabled())
        return false;

    const rtsp::SessionId session = RequestSession(*requestPtr);
    if(session.empty())
        return false;

    auto it = _p->mediaSessions.find(session);
    if(it == _p->mediaSessions.end())
        return false;

    MediaSession& mediaSession = *it->second;
    if(!mediaSession.recorder)
        return false;

    WebRTCPeer& localPeer = *(mediaSession.localPeer);

    localPeer.play();

    sendOkResponse(requestPtr->cseq, session);

    return true;
}

bool ServerSession::onTeardownRequest(
    std::unique_ptr<rtsp::Request>& requestPtr) noexcept
{
    const rtsp::SessionId session = RequestSession(*requestPtr);

    auto it = _p->mediaSessions.find(session);
    if(it == _p->mediaSessions.end())
        return false;

    WebRTCPeer& localPeer = *(it->second->localPeer);

    localPeer.stop();

    sendOkResponse(requestPtr->cseq, session);

    _p->mediaSessions.erase(it);

    return true;
}
