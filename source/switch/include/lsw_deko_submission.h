// Completion identities for the ONE ordered Deko queue. Presentation, guest
// frame serials and swapchain acquisition do not establish resource completion.
#pragma once
#include <cstdint>

namespace lsw_deko {
class SubmissionTimeline {
  uint64_t submitted_=0, completed_=0;
public:
  uint64_t next() const { return submitted_+1; }
  uint64_t submitted() const { return submitted_; }
  uint64_t completed() const { return completed_; }
  bool submit(uint64_t id) {
    if (!id || id!=next()) return false;
    submitted_=id; return true;
  }
  // Call ONLY after the fence associated with id returns success (or a proven
  // queue-idle result). A later fence covers all earlier work on this queue.
  bool observe_completion(uint64_t id) {
    if (!id || id>submitted_) return false;
    if (id>completed_) completed_=id;
    return true;
  }
  bool can_retire(uint64_t lastUse) const {
    // Zero/unsubmitted storage is not authorized by this GPU completion proof.
    return lastUse && lastUse<=completed_;
  }
};
} // namespace lsw_deko
