#include "quill/sinks/SinkBase.h"

namespace quill::detail
{
/**
 * Simple stdout sink
 */
class StdoutSink : public SinkBase
{
public:
  /**
   * Constructor
   */
  StdoutSink() = default;

  /**
   * Destructor
   * @param formatted_line
   */
  ~StdoutSink() override = default;

  /**
   * Log message to the sink
   * @param formatted_line
   */
  void log(fmt::memory_buffer const& formatted_line) override;
};
} // namespace quill::detail