/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2025 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "uci.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <optional>
#include <sstream>
#include <string_view>
#include <vector>

#include "benchmark.h"
#include "engine.h"
#include "memory.h"
#include "movegen.h"
#include "position.h"
#include "score.h"
#include "search.h"
#include "types.h"
#include "ucioption.h"

namespace Stockfish {

constexpr auto BenchmarkCommand = "speedtest";

constexpr auto StartFEN = "rnbakabnr/9/1c5c1/p1p1p1p1p/9/9/P1P1P1P1P/1C5C1/9/RNBAKABNR w";
template<typename... Ts>
struct overload: Ts... {
    using Ts::operator()...;
};

template<typename... Ts>
overload(Ts...) -> overload<Ts...>;

void UCIEngine::print_info_string(std::string_view str) {
    sync_cout_start();
    for (auto& line : split(str, "\n"))
    {
        if (!is_whitespace(line))
        {
            std::cout << "info string " << line << '\n';
        }
    }
    sync_cout_end();
}

UCIEngine::UCIEngine(int argc, char** argv) :
    engine(argv[0]),
    cli(argc, argv) {

    engine.get_options().add_info_listener([](const std::optional<std::string>& str) {
        if (str.has_value())
            print_info_string(*str);
    });

    init_search_update_listeners();
}

void UCIEngine::init_search_update_listeners() {
    engine.set_on_iter([](const auto& i) { on_iter(i); });
    engine.set_on_update_no_moves([](const auto& i) { on_update_no_moves(i); });
    engine.set_on_update_full(
      [this](const auto& i) { on_update_full(i, engine.get_options()["UCI_ShowWDL"]); });
    engine.set_on_bestmove([](const auto& bm, const auto& p) { on_bestmove(bm, p); });
    engine.set_on_verify_networks([](const auto& s) { print_info_string(s); });
}

// UCI引擎主循环函数，处理来自GUI或命令行的各种UCI指令
void UCIEngine::loop() {
    std::string token, cmd;

    // 将命令行参数拼接为命令字符串（允许通过命令行直接发送命令）
    for (int i = 1; i < cli.argc; ++i)
        cmd += std::string(cli.argv[i]) + " ";

    do {  // 主命令处理循环
        // 如果没有命令行参数，则从标准输入读取命令
        if (cli.argc == 1 && !getline(std::cin, cmd))  // 等待输入或EOF
            cmd = "quit";  // 遇到EOF时自动退出

        std::istringstream is(cmd);  // 使用字符串流解析命令

        token.clear();  // 清除前一个命令的token
        is >> std::skipws >> token;  // 跳过空白读取第一个token

        // 命令分发处理
        if (token == "quit" || token == "stop")  // 停止引擎命令
            engine.stop();

        // 处理"ponderhit"指令（当对手走出预期着法时触发）
        else if (token == "ponderhit")
            engine.set_ponderhit(false);  // 关闭ponder模式，转正常搜索

        // UCI协议初始化命令
        else if (token == "uci") {
            // 发送引擎信息（包含版本号、作者等）
            sync_cout << "id name " << engine_info(true) << "\n"
                      << engine.get_options() << sync_endl;  // 输出可配置参数
            sync_cout << "uciok" << sync_endl;  // 表示UCI初始化完成
        }

        // 设置引擎参数命令
        else if (token == "setoption")
            setoption(is);  // 调用参数设置函数
        // 开始搜索命令
        else if (token == "go") {
            print_info_string(engine.numa_config_information_as_string());
            print_info_string(engine.thread_allocation_information_as_string());
            go(is);  // 调用搜索处理函数
        }
        // 设置棋盘位置命令
        else if (token == "position")
            position(is);
        // 处理直接发送FEN或startpos的情况（非标准UCI，增强兼容性）
        else if (token == "fen" || token == "startpos")
            is.seekg(0), position(is);  // 重置流指针并处理位置
        // 新游戏命令（用于重置引擎状态）
        else if (token == "ucinewgame")
            engine.search_clear();  // 清除置换表等历史信息
        // 准备状态确认命令
        else if (token == "isready")
            sync_cout << "readyok" << sync_endl;

        // 以下为自定义调试命令（非UCI标准）
        else if (token == "flip")  // 翻转棋盘，使白方和黑方的位置互换，仅用于调试，例如用于发现评估对称性错误
            engine.flip();
        else if (token == "bench")  // 运行bench
            bench(is);
        else if (token == BenchmarkCommand)  // 运行speedtest
            benchmark(is);
        else if (token == "d")  // 可视化当前棋盘状态
            sync_cout << engine.visualize() << sync_endl;
        else if (token == "eval")  // 输出当前局面评估细节
            engine.trace_eval();
        else if (token == "compiler")  // 显示编译器信息
            sync_cout << compiler_info() << sync_endl;
        else if (token == "export_net") {  // 导出神经网络权重
            std::optional<std::string> file;
            std::string                f;
            if (is >> std::skipws >> f)
                file = f;
            engine.save_network(file);
        }
        // 帮助和许可信息
        else if (token == "--help" || token == "help" || token == "--license" || token == "license")
            sync_cout
              << "\nPikafish is a powerful xiangqi engine for playing and analyzing."
                 "\nIt is released as free software licensed under the GNU GPLv3 License."
                 "\nPikafish is normally used with a graphical user interface (GUI) and implements"
                 "\nthe Universal Chess Interface (UCI) protocol to communicate with a GUI, an API, etc."
                 "\nFor any further information, visit https://github.com/official-pikafish/Pikafish#readme"
                 "\nor read the corresponding README.md and Copying.txt files distributed along with this program.\n"
              << sync_endl;
        else if (!token.empty() && token[0] != '#') // 未知命令处理
            sync_cout << "Unknown command: '" << cmd << "'. Type help for more information."
                      << sync_endl;

    } while (token != "quit" && cli.argc == 1);  // 命令行模式单次执行，交互模式持续循环
}

Search::LimitsType UCIEngine::parse_limits(std::istream& is) {
    Search::LimitsType limits;
    std::string        token;

    limits.startTime = now();  // The search starts as early as possible

    while (is >> token)
        if (token == "searchmoves")  // Needs to be the last command on the line
            while (is >> token)
                limits.searchmoves.push_back(token);

        else if (token == "wtime")
            is >> limits.time[WHITE];
        else if (token == "btime")
            is >> limits.time[BLACK];
        else if (token == "winc")
            is >> limits.inc[WHITE];
        else if (token == "binc")
            is >> limits.inc[BLACK];
        else if (token == "movestogo")
            is >> limits.movestogo;
        else if (token == "depth")
            is >> limits.depth;
        else if (token == "nodes")
            is >> limits.nodes;
        else if (token == "movetime")
            is >> limits.movetime;
        else if (token == "mate")
            is >> limits.mate;
        else if (token == "perft")
            is >> limits.perft;
        else if (token == "infinite")
            limits.infinite = 1;
        else if (token == "ponder")
            limits.ponderMode = true;

    return limits;
}

void UCIEngine::go(std::istringstream& is) {

    Search::LimitsType limits = parse_limits(is);

    if (limits.perft)
        perft(limits);
    else
        engine.go(limits);
}

void UCIEngine::bench(std::istream& args) {
    std::string token;
    uint64_t    num, nodes = 0, cnt = 1;
    uint64_t    nodesSearched = 0;
    const auto& options       = engine.get_options();

    engine.set_on_update_full([&](const auto& i) {
        nodesSearched = i.nodes;
        on_update_full(i, options["UCI_ShowWDL"]);
    });

    std::vector<std::string> list = Benchmark::setup_bench(engine.fen(), args);

    num = count_if(list.begin(), list.end(),
                   [](const std::string& s) { return s.find("go ") == 0 || s.find("eval") == 0; });

    TimePoint elapsed = now();

    for (const auto& cmd : list)
    {
        std::istringstream is(cmd);
        is >> std::skipws >> token;

        if (token == "go" || token == "eval")
        {
            std::cerr << "\nPosition: " << cnt++ << '/' << num << " (" << engine.fen() << ")"
                      << std::endl;
            if (token == "go")
            {
                Search::LimitsType limits = parse_limits(is);

                if (limits.perft)
                    nodesSearched = perft(limits);
                else
                {
                    engine.go(limits);
                    engine.wait_for_search_finished();
                }

                nodes += nodesSearched;
                nodesSearched = 0;
            }
            else
                engine.trace_eval();
        }
        else if (token == "setoption")
            setoption(is);
        else if (token == "position")
            position(is);
        else if (token == "ucinewgame")
        {
            engine.search_clear();  // search_clear may take a while
            elapsed = now();
        }
    }

    elapsed = now() - elapsed + 1;  // Ensure positivity to avoid a 'divide by zero'

    dbg_print();

    std::cerr << "\n==========================="    //
              << "\nTotal time (ms) : " << elapsed  //
              << "\nNodes searched  : " << nodes    //
              << "\nNodes/second    : " << 1000 * nodes / elapsed << std::endl;

    // reset callback, to not capture a dangling reference to nodesSearched
    engine.set_on_update_full([&](const auto& i) { on_update_full(i, options["UCI_ShowWDL"]); });
}

void UCIEngine::benchmark(std::istream& args) {
    // Probably not very important for a test this long, but include for completeness and sanity.
    static constexpr int NUM_WARMUP_POSITIONS = 3;

    std::string token;
    uint64_t    nodes = 0, cnt = 1;
    uint64_t    nodesSearched = 0;

    engine.set_on_update_full([&](const Engine::InfoFull& i) { nodesSearched = i.nodes; });

    engine.set_on_iter([](const auto&) {});
    engine.set_on_update_no_moves([](const auto&) {});
    engine.set_on_bestmove([](const auto&, const auto&) {});
    engine.set_on_verify_networks([](const auto&) {});

    Benchmark::BenchmarkSetup setup = Benchmark::setup_benchmark(args);

    const int numGoCommands = count_if(setup.commands.begin(), setup.commands.end(),
                                       [](const std::string& s) { return s.find("go ") == 0; });

    TimePoint totalTime = 0;

    // Set options once at the start.
    auto ss = std::istringstream("name Threads value " + std::to_string(setup.threads));
    setoption(ss);
    ss = std::istringstream("name Hash value " + std::to_string(setup.ttSize));
    setoption(ss);

    // Warmup
    for (const auto& cmd : setup.commands)
    {
        std::istringstream is(cmd);
        is >> std::skipws >> token;

        if (token == "go")
        {
            // One new line is produced by the search, so omit it here
            std::cerr << "\rWarmup position " << cnt++ << '/' << NUM_WARMUP_POSITIONS;

            Search::LimitsType limits = parse_limits(is);

            TimePoint elapsed = now();

            // Run with silenced network verification
            engine.go(limits);
            engine.wait_for_search_finished();

            totalTime += now() - elapsed;

            nodes += nodesSearched;
            nodesSearched = 0;
        }
        else if (token == "position")
            position(is);
        else if (token == "ucinewgame")
        {
            engine.search_clear();  // search_clear may take a while
        }

        if (cnt > NUM_WARMUP_POSITIONS)
            break;
    }

    std::cerr << "\n";

    cnt   = 1;
    nodes = 0;

    int           numHashfullReadings = 0;
    constexpr int hashfullAges[]      = {0, 999};  // Only normal hashfull and touched hash.
    int           totalHashfull[std::size(hashfullAges)] = {0};
    int           maxHashfull[std::size(hashfullAges)]   = {0};

    auto updateHashfullReadings = [&]() {
        numHashfullReadings += 1;

        for (int i = 0; i < static_cast<int>(std::size(hashfullAges)); ++i)
        {
            const int hashfull = engine.get_hashfull(hashfullAges[i]);
            maxHashfull[i]     = std::max(maxHashfull[i], hashfull);
            totalHashfull[i] += hashfull;
        }
    };

    engine.search_clear();  // search_clear may take a while

    for (const auto& cmd : setup.commands)
    {
        std::istringstream is(cmd);
        is >> std::skipws >> token;

        if (token == "go")
        {
            // One new line is produced by the search, so omit it here
            std::cerr << "\rPosition " << cnt++ << '/' << numGoCommands;

            Search::LimitsType limits = parse_limits(is);

            TimePoint elapsed = now();

            // Run with silenced network verification
            engine.go(limits);
            engine.wait_for_search_finished();

            totalTime += now() - elapsed;

            updateHashfullReadings();

            nodes += nodesSearched;
            nodesSearched = 0;
        }
        else if (token == "position")
            position(is);
        else if (token == "ucinewgame")
        {
            engine.search_clear();  // search_clear may take a while
        }
    }

    totalTime = std::max<TimePoint>(totalTime, 1);  // Ensure positivity to avoid a 'divide by zero'

    dbg_print();

    std::cerr << "\n";

    static_assert(
      std::size(hashfullAges) == 2 && hashfullAges[0] == 0 && hashfullAges[1] == 999,
      "Hardcoded for display. Would complicate the code needlessly in the current state.");

    std::string threadBinding = engine.thread_binding_information_as_string();
    if (threadBinding.empty())
        threadBinding = "none";

    // clang-format off

    std::cerr << "==========================="
              << "\nVersion                    : "
              << engine_version_info()
              // "\nCompiled by                : "
              << compiler_info()
              << "Large pages                : " << (has_large_pages() ? "yes" : "no")
              << "\nUser invocation            : " << BenchmarkCommand << " "
              << setup.originalInvocation << "\nFilled invocation          : " << BenchmarkCommand
              << " " << setup.filledInvocation
              << "\nAvailable processors       : " << engine.get_numa_config_as_string()
              << "\nThread count               : " << setup.threads
              << "\nThread binding             : " << threadBinding
              << "\nTT size [MiB]              : " << setup.ttSize
              << "\nHash max, avg [per mille]  : "
              << "\n    single search          : " << maxHashfull[0] << ", "
              << totalHashfull[0] / numHashfullReadings
              << "\n    single game            : " << maxHashfull[1] << ", "
              << totalHashfull[1] / numHashfullReadings
              << "\nTotal nodes searched       : " << nodes
              << "\nTotal search time [s]      : " << totalTime / 1000.0
              << "\nNodes/second               : " << 1000 * nodes / totalTime << std::endl;

    // clang-format on

    init_search_update_listeners();
}

void UCIEngine::setoption(std::istringstream& is) {
    engine.wait_for_search_finished();
    engine.get_options().setoption(is);
}

std::uint64_t UCIEngine::perft(const Search::LimitsType& limits) {
    auto nodes = engine.perft(engine.fen(), limits.perft);
    sync_cout << "\nNodes searched: " << nodes << "\n" << sync_endl;
    return nodes;
}

void UCIEngine::position(std::istringstream& is) {
    std::string token, fen;

    is >> token;

    if (token == "startpos")
    {
        fen = StartFEN;
        is >> token;  // 吞掉moves字符串，不考虑
    }
    else if (token == "fen")
        while (is >> token && token != "moves")
            fen += token + " ";
    else
        return;

    std::vector<std::string> moves;

    while (is >> token)
    {
        moves.push_back(token);
    }

    engine.set_position(fen, moves);
}

namespace {

struct WinRateParams {
    double a;
    double b;
};

WinRateParams win_rate_params(const Position& pos) {

    int material = 10 * pos.count<ROOK>() + 5 * pos.count<KNIGHT>() + 5 * pos.count<CANNON>()
                 + 3 * pos.count<BISHOP>() + 2 * pos.count<ADVISOR>() + pos.count<PAWN>();

    // 拟合模型仅使用子力计数在[17, 110]范围内的数据，并以65为基准
    double m = std::clamp(material, 17, 110) / 65.0;

    // 返回 a = p_a(子力) 和 b = p_b(子力)，详见 github.com/official-stockfish/WDL_model
    constexpr double as[] = {220.59891365, -810.35730430, 928.68185198, 79.83955423};
    constexpr double bs[] = {61.99287416, -233.72674182, 325.85508322, -68.72720854};

    double a = (((as[0] * m + as[1]) * m + as[2]) * m) + as[3];
    double b = (((bs[0] * m + bs[1]) * m + bs[2]) * m) + bs[3];

    return {a, b};
}

// 胜率模型为 1 / (1 + exp((a - 评估值) / b))，其中 a = p_a(子力) 和 b = p_b(子力)
// 模型准确拟合了 LTC fishtest 统计数据
int win_rate_model(Value v, const Position& pos) {

    auto [a, b] = win_rate_params(pos);

    // 返回以千分单位表示的胜率，并四舍五入到最接近的整数
    return int(0.5 + 1000 / (1 + std::exp((a - double(v)) / b)));
}
}

std::string UCIEngine::format_score(const Score& s) {
    const auto format = overload{[](Score::Mate mate) -> std::string {
                                     auto m = (mate.plies > 0 ? (mate.plies + 1) : mate.plies) / 2;
                                     return std::string("mate ") + std::to_string(m);
                                 },
                                 [](Score::InternalUnits units) -> std::string {
                                     return std::string("cp ") + std::to_string(units.value);
                                 }};

    return s.visit(format);
}

// 将 Value 转换为整数厘兵（=百分之一兵）分值，
// 不处理绝杀和其他类似的特殊分数。
// 厘兵是国际象棋的一个分值单位，一个兵的价值为100厘兵，英语是Centipawn，简称cp（不是couple！）
int UCIEngine::to_cp(Value v, const Position& pos) {

    // 一般来说，得分可以通过 WDL 定义为
    // (log(1/L - 1) - log(1/W - 1)) / (log(1/L - 1) + log(1/W - 1))。
    // 根据我们的 win_rate_model，这简单表示成 v / a。

    auto [a, b] = win_rate_params(pos);

    return std::round(100 * int(v) / a);
}

std::string UCIEngine::wdl(Value v, const Position& pos) {
    std::stringstream ss;

    int wdl_w = win_rate_model(v, pos);
    int wdl_l = win_rate_model(-v, pos);
    int wdl_d = 1000 - wdl_w - wdl_l;
    ss << wdl_w << " " << wdl_d << " " << wdl_l;

    return ss.str();
}

// 将一个位置转换成字母+数字的表示形式，如a0
std::string UCIEngine::square(Square s) {
    return std::string{char('a' + file_of(s)), char('0' + rank_of(s))};
}

std::string UCIEngine::move(Move m) {
    if (m == Move::none())
        return "(none)";

    if (m == Move::null()) // 空着
        return "0000";

    Square from = m.from_sq();
    Square to   = m.to_sq();

    std::string move = square(from) + square(to);

    return move;
}

Move UCIEngine::to_move(const Position& pos, std::string str) {
    for (const auto& m : MoveList<LEGAL>(pos))
        if (str == move(m))
            return m;

    return Move::none();
}

void UCIEngine::on_update_no_moves(const Engine::InfoShort& info) {
    sync_cout << "info depth " << info.depth << " score " << format_score(info.score) << sync_endl;
}

void UCIEngine::on_update_full(const Engine::InfoFull& info, bool showWDL) {
    std::stringstream ss;

    ss << "info";
    ss << " depth " << info.depth                 // 深度（=层数）
       << " seldepth " << info.selDepth           // 选择性搜索深度，全称Selective Depth
       << " multipv " << info.multiPV             // 第几个着法
       << " score " << format_score(info.score);  // 分数

    if (showWDL)
        ss << " wdl " << info.wdl;

    if (!info.bound.empty())
        ss << " " << info.bound;

    ss << " nodes " << info.nodes        // 节点数
       << " nps " << info.nps            // 每秒搜索的节点数
       << " hashfull " << info.hashfull  // 代表置换表内的局面被替换或查询过的占比，千分数
       << " tbhits " << info.tbHits      // 置换表命中率
       << " time " << info.timeMs        // 搜索时间
       << " pv " << info.pv;             // 最佳走法序列（主要变例）

    sync_cout << ss.str() << sync_endl;
}

void UCIEngine::on_iter(const Engine::InfoIter& info) {
    std::stringstream ss;

    ss << "info";
    ss << " depth " << info.depth                     // 深度
       << " currmove " << info.currmove               // 当前正在搜索的着法
       << " currmovenumber " << info.currmovenumber;  // 着法序号

    sync_cout << ss.str() << sync_endl;
}

void UCIEngine::on_bestmove(std::string_view bestmove, std::string_view ponder) {
    sync_cout << "bestmove " << bestmove;
    if (!ponder.empty())
        std::cout << " ponder " << ponder;
    std::cout << sync_endl;
}

}  // namespace Stockfish
