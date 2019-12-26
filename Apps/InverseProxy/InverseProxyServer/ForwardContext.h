#pragma once

#include <string>
#include <memory>

#include <RtspParser/Request.h>
#include <RtspParser/Response.h>

class FrontSession;
class BackSession;


class ForwardContext
{
public:
    ForwardContext();
    ~ForwardContext();

    void registerFrontSession(FrontSession*);
    void removeFrontSession(FrontSession*);

    bool registerBackSession(const std::string& name, BackSession*);
    void removeBackSession(const std::string& name, BackSession*);

    bool forwardToBackSession(
        FrontSession*,
        std::unique_ptr<rtsp::Request>&);
    bool forwardToBackSession(
        FrontSession*,
        const rtsp::Request&,
        const rtsp::Response&);
    bool forwardToFrontSession(
        BackSession*,
        const rtsp::Request&);
    bool forwardToFrontSession(
        BackSession*,
        const rtsp::Request&,
        const rtsp::Response&);

private:
    struct Private;
    std::unique_ptr<Private> _p;
};
