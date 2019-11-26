#pragma once

#include <memory>
#include <functional>

#include "Streamer.h"


namespace streaming {

class GstStreamer : public Streamer
{
public:
    GstStreamer();
    ~GstStreamer();

    typedef std::function<void ()> PreparedCallback;
    void prepare(const PreparedCallback&) noexcept;
    bool sdp(std::string* sdp) noexcept override;

    void setRemoteSdp(const std::string& sdp) noexcept;

    void play() noexcept;

private:
    void eos(bool error);

private:
    struct Private;
    std::unique_ptr<Private> _p;
};

}
