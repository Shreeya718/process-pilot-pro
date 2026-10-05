#pragma once

namespace pilot {

/// Native command-line front end.
///
///   process-pilot validate <config>
///   process-pilot list     <config>
///   process-pilot start    <config>       (runs the supervisor in the foreground)
///   process-pilot status | stop | restart [service] | monitor
///
/// Options: --socket PATH  --log-file PATH  --log-level LEVEL  --interval MS  --count N
///
/// Exit codes: 0 success, 1 runtime/validation error, 2 usage error.
class Cli {
public:
    static int run(int argc, char** argv);
};

}  // namespace pilot
