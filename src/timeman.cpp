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

#include "timeman.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>

#include "search.h"
#include "ucioption.h"

namespace Stockfish {

TimePoint TimeManagement::optimum() const { return optimumTime; }
TimePoint TimeManagement::maximum() const { return maximumTime; }

void TimeManagement::clear() {
    availableNodes = -1;  // When in 'nodes as time' mode
}

void TimeManagement::advance_nodes_time(std::int64_t nodes) {
    assert(useNodesTime);
    availableNodes = std::max(int64_t(0), availableNodes - nodes);
}

// 时间管理模块的初始化函数，用于计算当前棋步允许的时间范围
// 参数:
//   limits: 搜索限制，包含步时、层数等信息
//   us: 当前玩家的颜色（红/黑）
//   ply: 步数
//   options: 引擎选项配置
//   originalTimeAdjust: 原始时间调整系数的引用，用于动态调整时间分配
void TimeManagement::init(Search::LimitsType& limits,
                          Color               us,
                          int                 ply,
                          const OptionsMap&   options,
                          double&             originalTimeAdjust) {
    // 获取"nodestime"选项值，表示将节点数转换为时间的系数（每毫秒对应的节点数）
    TimePoint npmsec = TimePoint(options["nodestime"]); // npmsec = nodes per millisecond

    // 初始化基础参数：记录搜索开始时间和是否使用节点时间模式
    startTime    = limits.startTime;        // 记录搜索开始的时间点
    useNodesTime = npmsec != 0;             // 判断是否启用了“节点作为时间”的模式

    // 如果没有时间限制则直接返回（例如无限模式）
    if (limits.time[us] == 0)
        return;

    // 获取移动开销（网络延迟/硬件延迟补偿）
    TimePoint moveOverhead = TimePoint(options["Move Overhead"]);

    // 定义时间分配系数：optScale为最佳时间比例，maxScale为最大时间系数
    double optScale, maxScale;

    // 节点时间模式处理：将实际时间转换为虚拟节点数进行管理
    if (useNodesTime)
    {
        // 仅在游戏开始时初始化可用节点总数
        if (availableNodes == -1)                     
            availableNodes = npmsec * limits.time[us];  // 总时间（毫秒）转换为节点数

        // 将所有时间参数转换为节点数单位
        limits.time[us] = TimePoint(availableNodes);    // 剩余时间转为节点数
        limits.inc[us] *= npmsec;                       // 每步增量转为节点数
        limits.npmsec = npmsec;                         // 存储转换系数
        moveOverhead *= npmsec;                         // 移动开销转为节点数
    }

    // 定义缩放因子：在节点时间模式下需要将时间参数还原为“虚拟毫秒”
    const int64_t   scaleFactor = useNodesTime ? npmsec : 1; // 时间缩放因子
    const TimePoint scaledTime  = limits.time[us] / scaleFactor; // 缩放后的剩余时间
    const TimePoint scaledInc   = limits.inc[us] / scaleFactor;  // 缩放后的每步增量

    // 计算剩余步数限制（mtg = moves to go），最大不超过60步
    int mtg = limits.movestogo ? std::min(limits.movestogo, 60) : 60;

    // 当剩余时间不足1秒时，动态调整剩余步数避免超时
    if (scaledTime < 1000 && double(mtg) / scaledInc > 0.05)
    {
        mtg = scaledTime * 0.05;  // 按时间比例缩减剩余步数
    }

    // 计算有效剩余时间（考虑增量和移动开销）
    TimePoint timeLeft = std::max(TimePoint(1), limits.time[us] + limits.inc[us] * (mtg - 1)
                                                  - moveOverhead * (2 + mtg));

    // 根据时间控制模式选择不同的时间分配策略
    // 模式1：局时加秒制（无固定步数要求）
    if (limits.movestogo == 0)
    {
        // 动态调整原始时间系数（基于对数函数的经验公式）
        if (originalTimeAdjust < 0)
            originalTimeAdjust = 0.3285 * std::log10(timeLeft) - 0.4830;

        // 计算时间分配常数（基于剩余时间的对数）
        double logTimeInSec = std::log10(scaledTime / 1000.0); // 转换为秒的对数
        double optConstant  = std::min(0.00344 + 0.000200 * logTimeInSec, 0.00450); // 最佳时间系数
        double maxConstant  = std::max(3.90 + 3.10 * logTimeInSec, 2.50);          // 最大时间系数

        // 计算最佳时间比例（考虑层数和动态调整系数）
        optScale = std::min(0.0155 + std::pow(ply + 3.0, 0.45) * optConstant,
                            0.2 * limits.time[us] / timeLeft)
                 * originalTimeAdjust;

        // 计算最大时间系数（随层数变化）
        maxScale = std::min(6.5, maxConstant + ply / 13.6);
    }
    // 模式2：固定步数时间控制（x步/y秒）
    else
    {
        // 最佳时间比例计算（考虑剩余步数和层数）
        optScale = std::min((0.88 + ply / 116.4) / mtg, 0.88 * limits.time[us] / timeLeft);
        // 最大时间系数计算（基于剩余步数的经验公式）
        maxScale = std::min(6.3, 1.5 + 0.11 * mtg);
    }

    // 计算最终时间分配值
    optimumTime = TimePoint(optScale * timeLeft); // 最佳使用时间
    maximumTime = TimePoint(std::min(0.81 * limits.time[us] - moveOverhead, maxScale * optimumTime)) - 10;

    // 如果开启后台思考（ponder），增加最佳时间的25%作为缓冲
    if (options["Ponder"])
        optimumTime += optimumTime / 4;
}

}  // namespace Stockfish
