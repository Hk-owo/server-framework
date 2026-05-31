//
// Created by lacas on 2026/3/11.
//

#ifndef WEBPROJECT_HTMLCONTENT_H
#define WEBPROJECT_HTMLCONTENT_H

namespace HtmlContent {

    // 使用单例模式管理HTML内容，避免重复构造导致的内存问题
    class HtmlManager {
    public:
        // 获取单例实例
        static HtmlManager &getInstance() {
            static HtmlManager instance; // C++11保证线程安全
            return instance;
        }
        std::string_view getMainWindowView();
        // 删除拷贝和移动操作，防止重复释放
        HtmlManager(const HtmlManager &) = delete;

        HtmlManager &operator=(const HtmlManager &) = delete;

        HtmlManager(HtmlManager &&) = delete;

        HtmlManager &operator=(HtmlManager &&) = delete;

        // 获取带变量替换的HTML
//        std::string getMainWindowWithVars(const std::unordered_map<std::string, std::string> &vars) const {
//            std::string result = kMainWindowHtml;
//            for (const auto &[key, value]: vars) {
//                size_t pos = 0;
//                while ((pos = result.find(key, pos)) != std::string::npos) {
//                    result.replace(pos, key.length(), value);
//                    pos += value.length();
//                }
//            }
//            return result;
//        }
    private:
        HtmlManager() = default;

        ~HtmlManager() = default;

        // 使用静态常量字符数组，避免std::string的动态内存分配
        // 这样更安全，不会在程序结束时触发析构
        static constexpr const char kMainWindowHtml[] = R"delimiter(
            <!DOCTYPE html>
            <html lang="zh-CN">
            <head>
                <meta charset="UTF-8">
                <meta name="viewport" content="width=device-width, initial-scale=1.0">
                <title>茶饮店管理系统 | 工作台</title>
                <script src="https://cdn.tailwindcss.com"></script>
                <link href="https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.4.0/css/all.min.css" rel="stylesheet">
                <link href="https://fonts.googleapis.com/css2?family=Noto+Sans+SC:wght@300;400;500;600;700&display=swap" rel="stylesheet">
                <link rel="stylesheet" href="/css/style.css">
            </head>
            <body class="h-screen flex overflow-hidden">

            <!-- 侧边栏 -->
            <aside class="sidebar w-64 h-full flex flex-col shadow-lg z-20">
                <div class="p-6 border-b border-amber-200">
                    <div class="flex items-center gap-3">
                        <div class="w-10 h-10 rounded-full bg-gradient-to-tr from-amber-400 to-orange-500 flex items-center justify-center shadow-md">
                            <i class="fas fa-mug-hot text-white text-lg"></i>
                        </div>
                        <div>
                            <h1 class="font-bold text-lg text-amber-900">茶饮店管理</h1>
                            <p class="text-xs text-amber-700/60">智能经营助手</p>
                        </div>
                    </div>
                </div>

                <nav class="flex-1 overflow-y-auto py-4">
                    <div class="px-4 mb-2 text-xs font-semibold text-amber-700/50 uppercase tracking-wider">主要功能</div>

                    <a href="#" class="nav-item active flex items-center gap-3 px-6 py-3 text-amber-800" onclick="switchTab('dashboard', this)">
                        <i class="fas fa-home w-5"></i>
                        <span>工作台</span>
                    </a>

                    <a href="#" class="nav-item flex items-center gap-3 px-6 py-3 text-amber-800" onclick="switchTab('pos', this)">
                        <i class="fas fa-cash-register w-5"></i>
                        <span>收银点单</span>
                        <span class="ml-auto bg-red-500 text-white text-xs px-2 py-0.5 rounded-full">Hot</span>
                    </a>

                    <a href="#" class="nav-item flex items-center gap-3 px-6 py-3 text-amber-800" onclick="switchTab('orders', this)">
                        <i class="fas fa-clipboard-list w-5"></i>
                        <span>订单管理</span>
                    </a>

                    <a href="#" class="nav-item flex items-center gap-3 px-6 py-3 text-amber-800" onclick="switchTab('members', this)">
                        <i class="fas fa-users w-5"></i>
                        <span>会员中心</span>
                    </a>

                    <div class="px-4 mt-6 mb-2 text-xs font-semibold text-amber-700/50 uppercase tracking-wider">库存采购</div>

                    <a href="#" class="nav-item flex items-center gap-3 px-6 py-3 text-amber-800" onclick="switchTab('inventory', this)">
                        <i class="fas fa-boxes w-5"></i>
                        <span>库存管理</span>
                    </a>

                    <a href="#" class="nav-item flex items-center gap-3 px-6 py-3 text-amber-800" onclick="switchTab('purchase', this)">
                        <i class="fas fa-shopping-cart w-5"></i>
                        <span>采购管理</span>
                    </a>

                    <div class="px-4 mt-6 mb-2 text-xs font-semibold text-amber-700/50 uppercase tracking-wider">数据报表</div>

                    <a href="#" class="nav-item flex items-center gap-3 px-6 py-3 text-amber-800" onclick="switchTab('reports', this)">
                        <i class="fas fa-chart-line w-5"></i>
                        <span>营业报表</span>
                    </a>
                </nav>

                <div class="p-4 border-t border-amber-200 bg-amber-50/50">
                    <button onclick="logout()" class="w-full py-2 px-4 rounded-lg border border-amber-300 text-amber-700 text-sm hover:bg-amber-100 transition-colors flex items-center justify-center gap-2">
                        <i class="fas fa-sign-out-alt"></i>
                        退出登录
                    </button>
                </div>
            </aside>

            <!-- 主内容区 -->
            <main class="flex-1 flex flex-col h-full overflow-hidden">
                <header class="bg-white border-b border-amber-200 h-16 flex items-center justify-between px-8 shadow-sm">
                    <div class="flex items-center gap-4">
                        <h2 class="text-xl font-bold text-amber-900" id="pageTitle">工作台</h2>
                        <span class="text-sm text-amber-600 bg-amber-100 px-3 py-1 rounded-full" id="currentDate"></span>
                    </div>

                    <div class="flex items-center gap-4">
                        <div class="relative">
                            <input type="text" placeholder="搜索订单、会员、商品..."
                                   class="pl-10 pr-4 py-2 w-64 rounded-lg border border-amber-200 focus:outline-none focus:border-amber-500 focus:ring-1 focus:ring-amber-500 text-sm bg-amber-50/30">
                            <i class="fas fa-search absolute left-3 top-2.5 text-amber-400"></i>
                        </div>
                        <button class="relative p-2 text-amber-600 hover:bg-amber-100 rounded-lg transition-colors" onclick="showNotifications()">
                            <i class="fas fa-bell text-xl"></i>
                            <span class="absolute top-1 right-1 w-2 h-2 bg-red-500 rounded-full animate-pulse"></span>
                        </button>
                    </div>
                </header>

                <div class="flex-1 overflow-y-auto p-8" id="mainContent">

                    <!-- 工作台 -->
                    <div id="dashboard" class="space-y-6 animate-fade-in">
                        <div class="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-4 gap-6">
                            <div class="stat-card rounded-xl p-6">
                                <div class="flex justify-between items-start">
                                    <div>
                                        <p class="text-sm text-amber-700/60 mb-1">今日营业额</p>
                                        <h3 class="text-2xl font-bold text-amber-900" id="todayRevenue">¥0.00</h3>
                                        <p class="text-xs text-green-600 mt-2 flex items-center gap-1">
                                            <i class="fas fa-arrow-up"></i>
                                            <span id="revenueGrowth">0%</span> 较昨日
                                        </p>
                                    </div>
                                    <div class="w-12 h-12 rounded-lg bg-gradient-to-br from-amber-400 to-orange-500 flex items-center justify-center text-white shadow-lg">
                                        <i class="fas fa-yen-sign text-xl"></i>
                                    </div>
                                </div>
                            </div>

                            <div class="stat-card rounded-xl p-6">
                                <div class="flex justify-between items-start">
                                    <div>
                                        <p class="text-sm text-amber-700/60 mb-1">今日订单数</p>
                                        <h3 class="text-2xl font-bold text-amber-900" id="todayOrders">0</h3>
                                        <p class="text-xs text-green-600 mt-2 flex items-center gap-1">
                                            <i class="fas fa-arrow-up"></i>
                                            <span id="ordersGrowth">0%</span> 较昨日
                                        </p>
                                    </div>
                                    <div class="w-12 h-12 rounded-lg bg-gradient-to-br from-blue-400 to-blue-600 flex items-center justify-center text-white shadow-lg">
                                        <i class="fas fa-shopping-bag text-xl"></i>
                                    </div>
                                </div>
                            </div>

                            <div class="stat-card rounded-xl p-6">
                                <div class="flex justify-between items-start">
                                    <div>
                                        <p class="text-sm text-amber-700/60 mb-1">会员新增</p>
                                        <h3 class="text-2xl font-bold text-amber-900" id="newMembers">0</h3>
                                        <p class="text-xs text-amber-600 mt-2">累计会员 <span id="totalMembers">0</span></p>
                                    </div>
                                    <div class="w-12 h-12 rounded-lg bg-gradient-to-br from-purple-400 to-purple-600 flex items-center justify-center text-white shadow-lg">
                                        <i class="fas fa-user-plus text-xl"></i>
                                    </div>
                                </div>
                            </div>

                            <div class="stat-card rounded-xl p-6">
                                <div class="flex justify-between items-start">
                                    <div>
                                        <p class="text-sm text-amber-700/60 mb-1">库存预警</p>
                                        <h3 class="text-2xl font-bold text-red-600" id="lowStockCount">0</h3>
                                        <p class="text-xs text-red-500 mt-2 cursor-pointer hover:underline" onclick="switchTab('inventory', document.querySelectorAll('.nav-item')[4])">查看详情 →</p>
                                    </div>
                                    <div class="w-12 h-12 rounded-lg bg-gradient-to-br from-red-400 to-red-600 flex items-center justify-center text-white shadow-lg">
                                        <i class="fas fa-exclamation-triangle text-xl"></i>
                                    </div>
                                </div>
                            </div>
                        </div>

                        <div class="grid grid-cols-1 lg:grid-cols-3 gap-6">
                            <div class="lg:col-span-1 space-y-4">
                                <h3 class="font-bold text-amber-900 text-lg">快捷操作</h3>
                                <div class="grid grid-cols-2 gap-4">
                                    <button onclick="switchTab('pos', document.querySelectorAll('.nav-item')[1])" class="quick-action rounded-xl p-6 text-center">
                                        <div class="w-14 h-14 mx-auto mb-3 rounded-full bg-amber-100 flex items-center justify-center text-amber-600 text-2xl">
                                            <i class="fas fa-plus"></i>
                                        </div>
                                        <p class="font-medium text-amber-900">新建订单</p>
                                        <p class="text-xs text-amber-600 mt-1">快速收银</p>
                                    </button>

                                    <button onclick="openMemberModal()" class="quick-action rounded-xl p-6 text-center">
                                        <div class="w-14 h-14 mx-auto mb-3 rounded-full bg-purple-100 flex items-center justify-center text-purple-600 text-2xl">
                                            <i class="fas fa-user-plus"></i>
                                        </div>
                                        <p class="font-medium text-amber-900">新增会员</p>
                                        <p class="text-xs text-amber-600 mt-1">注册会员</p>
                                    </button>

                                    <button onclick="openPurchaseModal()" class="quick-action rounded-xl p-6 text-center">
                                        <div class="w-14 h-14 mx-auto mb-3 rounded-full bg-blue-100 flex items-center justify-center text-blue-600 text-2xl">
                                            <i class="fas fa-cart-plus"></i>
                                        </div>
                                        <p class="font-medium text-amber-900">采购入库</p>
                                        <p class="text-xs text-amber-600 mt-1">添加库存</p>
                                    </button>

                                    <button onclick="switchTab('reports', document.querySelectorAll('.nav-item')[6])" class="quick-action rounded-xl p-6 text-center">
                                        <div class="w-14 h-14 mx-auto mb-3 rounded-full bg-green-100 flex items-center justify-center text-green-600 text-2xl">
                                            <i class="fas fa-chart-pie"></i>
                                        </div>
                                        <p class="font-medium text-amber-900">数据报表</p>
                                        <p class="text-xs text-amber-600 mt-1">查看统计</p>
                                    </button>
                                </div>

                                <div class="bg-white rounded-xl border border-amber-200 p-6 mt-6">
                                    <h4 class="font-bold text-amber-900 mb-4">今日热销 TOP5</h4>
                                    <div class="space-y-3" id="topProducts">
                                        <!-- API: GET /api/v_product_rank?limit=5 -->
                                    </div>
                                </div>
                            </div>
                            <div class="lg:col-span-2 space-y-6">
                                <div class="chart-container">
                                    <div class="flex justify-between items-center mb-4">
                                        <h3 class="font-bold text-amber-900">营业额趋势</h3>
                                        <select class="text-sm border border-amber-200 rounded-lg px-3 py-1 bg-amber-50 focus:outline-none focus:border-amber-500"
                                                onchange="loadRevenueChart(parseInt(this.value))">
                                            <option value="7">最近7天</option>
                                            <option value="30">最近30天</option>
                                        </select>
                                    </div>
                                    <div class="h-64 flex items-center justify-center" id="revenueChart">
                                        <!-- 初始加载状态 -->
                                        <div class="flex flex-col items-center text-amber-600">
                                            <i class="fas fa-spinner fa-spin text-2xl mb-2"></i>
                                            <span class="text-sm">加载图表...</span>
                                        </div>
                                    </div>
                                </div>

                                <div class="bg-white rounded-xl border border-amber-200 overflow-hidden">
                                    <div class="p-6 border-b border-amber-100 flex justify-between items-center">
                                        <h3 class="font-bold text-amber-900">最近订单</h3>
                                        <button onclick="switchTab('orders', document.querySelectorAll('.nav-item')[2])" class="text-sm text-amber-600 hover:text-amber-700 font-medium">查看全部 →</button>
                                    </div>
                                    <div class="overflow-x-auto">
                                        <table class="w-full data-table">
                                            <thead>
                                            <tr>
                                                <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">订单号</th>
                                                <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">会员</th>
                                                <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">商品</th>
                                                <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">金额</th>
                                                <th class="text-left py-3 px-6 text-xs uppercase tracking-wider w-28">状态</th>
                                                <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">时间</th>
                                                <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">操作</th>
                                            </tr>
                                            </thead>
                                            <tbody id="recentOrders">
                                                <!-- 初始加载状态 -->
                                                <tr>
                                                    <td colspan="7" class="text-center py-8 text-amber-600">
                                                        <i class="fas fa-spinner fa-spin text-xl mb-2 block mx-auto"></i>
                                                        <span class="text-sm">加载订单...</span>
                                                    </td>
                                                </tr>
                                            </tbody>
                                        </table>
                                    </div>
                                </div>
                            </div>
                        </div>
                    </div>

                    <!-- 收银点单 (POS) -->
                    <div id="pos" class="hidden animate-fade-in h-full">
                        <div class="flex h-full gap-6">
                            <!-- 商品选择区 -->
                            <div class="flex-1 flex flex-col">
                                <!-- 分类筛选 -->
                                <div class="flex gap-2 mb-4 overflow-x-auto pb-2">
                                    <button data-category="all" onclick="filterProducts('all')"
                                            class="category-btn px-4 py-2 rounded-full bg-amber-500 text-white text-sm font-medium whitespace-nowrap">全部</button>
                                    <button data-category="奶茶" onclick="filterProducts('奶茶')"
                                            class="category-btn px-4 py-2 rounded-full bg-white border border-amber-200 text-amber-700 text-sm font-medium whitespace-nowrap hover:bg-amber-50">奶茶</button>
                                    <button data-category="果茶" onclick="filterProducts('果茶')"
                                            class="category-btn px-4 py-2 rounded-full bg-white border border-amber-200 text-amber-700 text-sm font-medium whitespace-nowrap hover:bg-amber-50">果茶</button>
                                    <button data-category="咖啡" onclick="filterProducts('咖啡')"
                                            class="category-btn px-4 py-2 rounded-full bg-white border border-amber-200 text-amber-700 text-sm font-medium whitespace-nowrap hover:bg-amber-50">咖啡</button>
                                    <button data-category="甜品" onclick="filterProducts('甜品')"
                                            class="category-btn px-4 py-2 rounded-full bg-white border border-amber-200 text-amber-700 text-sm font-medium whitespace-nowrap hover:bg-amber-50">甜品</button>
                                </div>

                                <!-- 商品网格 -->
                                <div class="flex-1 overflow-y-auto">
                                    <div class="grid grid-cols-3 lg:grid-cols-4 gap-4" id="productGrid">
                                        <!-- API: GET /api/product?status=1 -->
                                    </div>
                                </div>
                            </div>

                            <!-- 购物车侧边栏 -->
                            <div class="w-96 bg-white rounded-xl border border-amber-200 flex flex-col shadow-lg">
                                <div class="p-4 border-b border-amber-200 bg-amber-50/50 rounded-t-xl">
                                    <h3 class="font-bold text-amber-900 flex items-center gap-2">
                                        <i class="fas fa-shopping-cart"></i>
                                        当前订单
                                    </h3>
                                </div>

                                <!-- 会员信息 -->
                                <div class="p-4 border-b border-amber-100">
                                    <div class="flex items-center gap-2 mb-2">
                                        <i class="fas fa-user text-amber-600"></i>
                                        <span class="text-sm font-medium text-amber-900">会员</span>
                                    </div>
                                    <div class="flex gap-2">
                                        <input type="text" id="posMemberPhone" placeholder="输入手机号查询会员"
                                               class="flex-1 px-3 py-2 rounded-lg border border-amber-200 text-sm focus:outline-none focus:border-amber-500">
                                        <button onclick="queryMember()" class="px-4 py-2 bg-amber-500 text-white rounded-lg text-sm hover:bg-amber-600">
                                            查询
                                        </button>
                                    </div>
                                    <div id="posMemberInfo" class="hidden mt-3 p-3 bg-amber-50 rounded-lg">
                                        <!-- 显示会员信息 -->
                                    </div>
                                </div>

                                <!-- 购物车列表 -->
                                <div class="flex-1 overflow-y-auto p-4" id="cartItems">
                                    <div class="text-center text-amber-400 py-8">
                                        <i class="fas fa-coffee text-4xl mb-2"></i>
                                        <p>请选择商品</p>
                                    </div>
                                </div>

                                <!-- 结算区 -->
                                <div class="p-4 border-t border-amber-200 bg-amber-50/30 rounded-b-xl">
                                    <div class="space-y-2 mb-4">
                                        <div class="flex justify-between text-sm">
                                            <span class="text-amber-700">商品总额</span>
                                            <span class="font-medium text-amber-900" id="cartSubtotal">¥0.00</span>
                                        </div>
                                        <div class="flex justify-between text-sm">
                                            <span class="text-amber-700">会员优惠</span>
                                            <span class="font-medium text-red-500" id="cartDiscount">-¥0.00</span>
                                        </div>
                                        <div class="flex justify-between text-lg font-bold border-t border-amber-200 pt-2">
                                            <span class="text-amber-900">应付金额</span>
                                            <span class="text-amber-600 text-2xl" id="cartTotal">¥0.00</span>
                                        </div>
                                    </div>

                                    <div class="grid grid-cols-2 gap-2 mb-3">
                                        <button onclick="setPayType(1)" class="pay-type-btn py-2 rounded-lg border border-amber-200 text-sm font-medium text-amber-700 hover:bg-amber-100" data-type="1">
                                            <i class="fas fa-money-bill-wave mr-1"></i>现金
                                        </button>
                                        <button onclick="setPayType(2)" class="pay-type-btn py-2 rounded-lg border border-amber-200 text-sm font-medium text-amber-700 hover:bg-amber-100" data-type="2">
                                            <i class="fab fa-weixin mr-1 text-green-600"></i>微信
                                        </button>
                                        <button onclick="setPayType(3)" class="pay-type-btn py-2 rounded-lg border border-amber-200 text-sm font-medium text-amber-700 hover:bg-amber-100" data-type="3">
                                            <i class="fab fa-alipay mr-1 text-blue-500"></i>支付宝
                                        </button>
                                        <button onclick="setPayType(4)" class="pay-type-btn py-2 rounded-lg border border-amber-200 text-sm font-medium text-amber-700 hover:bg-amber-100" data-type="4">
                                            <i class="fas fa-wallet mr-1 text-amber-600"></i>余额
                                        </button>
                                    </div>

                                    <button onclick="submitOrder()" class="w-full py-3 bg-gradient-to-r from-amber-500 to-orange-500 text-white rounded-lg font-bold text-lg hover:from-amber-600 hover:to-orange-600 transition-all shadow-lg">
                                        确认收款
                                    </button>
                                </div>
                            </div>
                        </div>
                    </div>

                    <!-- 订单管理 -->
                    <div id="orders" class="hidden animate-fade-in">
                        <div class="bg-white rounded-xl border border-amber-200 overflow-hidden">
                            <div class="p-6 border-b border-amber-100 flex justify-between items-center">
                                <div class="flex items-center gap-4">
                                    <h3 class="font-bold text-amber-900 text-lg">订单列表</h3>
                                    <div class="flex gap-2">
                                        <button onclick="filterOrders('all')" class="px-3 py-1 rounded-full bg-amber-500 text-white text-sm">全部</button>
                                        <button onclick="filterOrders('today')" class="px-3 py-1 rounded-full bg-amber-100 text-amber-700 text-sm hover:bg-amber-200">今日</button>
                                        <button onclick="filterOrders('week')" class="px-3 py-1 rounded-full bg-amber-100 text-amber-700 text-sm hover:bg-amber-200">本周</button>
                                    </div>
                                </div>
                                <button onclick="exportOrders()" class="px-4 py-2 border border-amber-300 text-amber-700 rounded-lg text-sm hover:bg-amber-50">
                                    <i class="fas fa-download mr-2"></i>导出
                                </button>
                            </div>
                            <div class="overflow-x-auto">
                                <table class="w-full data-table">
                                    <thead>
                                    <tr>
                                        <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">订单号</th>
                                        <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">时间</th>
                                        <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">会员</th>
                                        <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">商品</th>
                                        <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">金额</th>
                                        <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">优惠</th>
                                        <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">实付</th>
                                        <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">支付方式</th>
                                        <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">收银员</th>
                                        <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">操作</th>
                                    </tr>
                                    </thead>
                                    <tbody id="ordersTableBody">
                                    <!-- API: GET /api/sale_order/list -->
                                    </tbody>
                                </table>
                            </div>
                            <div class="p-4 border-t border-amber-100 flex justify-between items-center">
                                <span class="text-sm text-amber-600">共 <span id="ordersTotal">0</span> 条记录</span>
                                <div class="flex gap-2">
                                    <button onclick="changePage(-1)" class="px-3 py-1 rounded-lg border border-amber-200 text-amber-700 hover:bg-amber-50">
                                        <i class="fas fa-chevron-left"></i>
                                    </button>
                                    <span class="px-3 py-1 text-amber-700" id="pageInfo">1 / 1</span>
                                    <button onclick="changePage(1)" class="px-3 py-1 rounded-lg border border-amber-200 text-amber-700 hover:bg-amber-50">
                                        <i class="fas fa-chevron-right"></i>
                                    </button>
                                </div>
                            </div>
                        </div>
                    </div>

                    <!-- 会员中心 -->
                    <div id="members" class="hidden animate-fade-in">
                        <div class="flex gap-6 h-full">
                            <div class="flex-1">
                                <div class="bg-white rounded-xl border border-amber-200 overflow-hidden">
                                    <div class="p-6 border-b border-amber-100 flex justify-between items-center">
                                        <div class="flex items-center gap-4">
                                            <h3 class="font-bold text-amber-900 text-lg">会员列表</h3>
                                            <div class="relative">
                                                <!-- 搜索框 -->
                                                <input type="text" id="memberSearch" placeholder="搜索手机号/姓名"
                                                       class="pl-10 pr-4 py-2 w-64 rounded-lg border border-amber-200 text-sm focus:outline-none focus:border-amber-500">
                                                <i class="fas fa-search absolute left-3 top-2.5 text-amber-400"></i>
                                            </div>
                                        </div>
                                        <button onclick="openMemberModal()" class="px-4 py-2 bg-amber-500 text-white rounded-lg text-sm hover:bg-amber-600">
                                            <i class="fas fa-plus mr-2"></i>新增会员
                                        </button>
                                    </div>
                                    <div class="overflow-x-auto">
                                        <table class="w-full data-table">
                                            <thead>
                                            <tr>
                                                <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">会员ID</th>
                                                <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">手机号</th>
                                                <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">姓名</th>
                                                <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">等级</th>
                                                <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">储值余额</th>
                                                <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">积分</th>
                                                <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">累计消费</th>
                                                <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">状态</th>
                                                <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">注册时间</th>
                                                <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">操作</th>
                                            </tr>
                                            </thead>
                                            <tbody id="membersTableBody">
                                            <!-- 动态填充 -->
                                            </tbody>
                                        </table>
                                    </div>
                                </div>
                            </div>
                        </div>
                    </div>

                    <!-- 库存管理 -->
                    <div id="inventory" class="hidden animate-fade-in">
                        <div class="grid grid-cols-1 lg:grid-cols-3 gap-6">
                            <div class="lg:col-span-2">
                                <div class="bg-white rounded-xl border border-amber-200 overflow-hidden">
                                    <div class="p-6 border-b border-amber-100 flex justify-between items-center">
                                        <h3 class="font-bold text-amber-900 text-lg">库存清单</h3>
                                        <div class="flex gap-2">
                                            <button onclick="showLowStock()" class="px-4 py-2 bg-red-100 text-red-700 rounded-lg text-sm hover:bg-red-200">
                                                <i class="fas fa-exclamation-triangle mr-2"></i>预警商品
                                            </button>
                                            <button onclick="openStockAdjustModal()" class="px-4 py-2 bg-amber-500 text-white rounded-lg text-sm hover:bg-amber-600">
                                                <i class="fas fa-edit mr-2"></i>盘点调整
                                            </button>
                                        </div>
                                    </div>
                                    <div class="overflow-x-auto">
                                        <table class="w-full data-table">
                                            <thead>
                                            <tr>
                                                <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">原料ID</th>
                                                <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">原料名称</th>
                                                <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">单位</th>
                                                <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">当前库存</th>
                                                <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">安全库存</th>
                                                <th class="text-left py-3 px-6 text-xs uppercase tracking-wider w-24">状态</th>
                                                <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">更新时间</th>
                                                <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">操作</th>
                                            </tr>
                                            </thead>
                                            <tbody id="inventoryTableBody">
                                            <!-- API: GET /api/material/list -->
                                            </tbody>
                                        </table>
                                    </div>
                                </div>
                            </div>

                            <div class="lg:col-span-1">
                                <div class="bg-white rounded-xl border border-amber-200 p-6">
                                    <h3 class="font-bold text-amber-900 mb-4">库存流水</h3>
                                    <div class="space-y-3 max-h-96 overflow-y-auto" id="inventoryLogs">
                                        <!-- API: GET /api/inventory_log/recent -->
                                    </div>
                                </div>
                            </div>
                        </div>
                    </div>

                    <!-- 采购管理 -->
                    <div id="purchase" class="hidden animate-fade-in">
                        <div class="bg-white rounded-xl border border-amber-200 overflow-hidden">
                            <div class="p-6 border-b border-amber-100 flex justify-between items-center">
                                <h3 class="font-bold text-amber-900 text-lg">采购单列表</h3>
                                <button onclick="openPurchaseModal()" class="px-4 py-2 bg-amber-500 text-white rounded-lg text-sm hover:bg-amber-600">
                                    <i class="fas fa-plus mr-2"></i>新建采购单
                                </button>
                            </div>
                            <div class="overflow-x-auto">
                                <table class="w-full data-table">
                                    <thead>
                                    <tr>
                                        <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">采购单号</th>
                                        <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">供应商</th>
                                        <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">总金额</th>
                                        <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">状态</th>
                                        <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">操作人</th>
                                        <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">创建时间</th>
                                        <th class="text-left py-3 px-6 text-xs uppercase tracking-wider">操作</th>
                                    </tr>
                                    </thead>
                                    <tbody id="purchaseTableBody">
                                    <!-- API: GET /api/purchase_order/list -->
                                    </tbody>
                                </table>
                            </div>
                        </div>
                    </div>

                    <!-- 数据报表 -->
                    <div id="reports" class="hidden animate-fade-in">
                        <div class="grid grid-cols-1 lg:grid-cols-2 gap-6">
                            <div class="chart-container">
                                <h3 class="font-bold text-amber-900 mb-4">销售日报</h3>
                                <div class="overflow-x-auto">
                                    <table class="w-full data-table">
                                        <thead>
                                        <tr>
                                            <th class="text-left py-3 px-4 text-xs uppercase tracking-wider">日期</th>
                                            <th class="text-left py-3 px-4 text-xs uppercase tracking-wider">订单数</th>
                                            <th class="text-left py-3 px-4 text-xs uppercase tracking-wider">总金额</th>
                                            <th class="text-left py-3 px-4 text-xs uppercase tracking-wider">优惠</th>
                                            <th class="text-left py-3 px-4 text-xs uppercase tracking-wider">实收</th>
                                            <th class="text-left py-3 px-4 text-xs uppercase tracking-wider">现金</th>
                                            <th class="text-left py-3 px-4 text-xs uppercase tracking-wider">移动支付</th>
                                        </tr>
                                        </thead>
                                        <tbody id="dailyReportBody">
                                        <!-- API: GET /api/v_daily_sales -->
                                        </tbody>
                                    </table>
                                </div>
                            </div>

                            <div class="chart-container">
                                <h3 class="font-bold text-amber-900 mb-4">商品销售排行</h3>
                                <div class="space-y-3" id="productRankList">
                                    <!-- API: GET /api/v_product_rank -->
                                </div>
                            </div>
                        </div>
                    </div>

                </div>
            </main>

            <!-- 新增会员模态框 -->
            <div id="memberModal" class="fixed inset-0 modal-backdrop hidden z-50 flex items-center justify-center">
                <div class="bg-white rounded-2xl w-full max-w-md mx-4 shadow-2xl transform scale-95 opacity-0 transition-all duration-300" id="memberModalContent">
                    <div class="p-6 border-b border-amber-200 flex justify-between items-center">
                        <h3 class="font-bold text-amber-900 text-xl">新增会员</h3>
                        <button onclick="closeMemberModal()" class="text-amber-600 hover:text-amber-800">
                            <i class="fas fa-times text-xl"></i>
                        </button>
                    </div>
                    <div class="p-6 space-y-4">
                        <div>
                            <label class="block text-sm font-medium text-amber-800 mb-2">手机号 <span class="text-red-500">*</span></label>
                            <input type="tel" id="newMemberPhone" class="w-full px-4 py-2 rounded-lg border border-amber-200 focus:outline-none focus:border-amber-500 focus:ring-1 focus:ring-amber-500" placeholder="请输入手机号" maxlength="11">
                        </div>
                        <div>
                            <label class="block text-sm font-medium text-amber-800 mb-2">姓名</label>
                            <input type="text" id="newMemberName" class="w-full px-4 py-2 rounded-lg border border-amber-200 focus:outline-none focus:border-amber-500 focus:ring-1 focus:ring-amber-500" placeholder="请输入姓名">
                        </div>
                        <div>
                            <label class="block text-sm font-medium text-amber-800 mb-2">会员等级</label>
                            <select id="newMemberLevel" class="w-full px-4 py-2 rounded-lg border border-amber-200 focus:outline-none focus:border-amber-500 focus:ring-1 focus:ring-amber-500">
                                <option value="1">普通会员</option>
                                <option value="2">银卡会员</option>
                                <option value="3">金卡会员</option>
                            </select>
                        </div>
                        <div>
                            <label class="block text-sm font-medium text-amber-800 mb-2">初始储值 (元)</label>
                            <input type="number" id="newMemberBalance" class="w-full px-4 py-2 rounded-lg border border-amber-200 focus:outline-none focus:border-amber-500 focus:ring-1 focus:ring-amber-500" placeholder="0.00" min="0" step="0.01">
                        </div>
                    </div>
                    <div class="p-6 border-t border-amber-200 flex gap-3">
                        <button onclick="closeMemberModal()" class="flex-1 py-2 px-4 rounded-lg border border-amber-300 text-amber-700 hover:bg-amber-50 transition-colors">取消</button>
                        <button onclick="submitNewMember()" class="flex-1 py-2 px-4 rounded-lg btn-primary">确认添加</button>
                    </div>
                </div>
            </div>

            <!-- 采购入库模态框 -->
            <div id="purchaseModal" class="fixed inset-0 modal-backdrop hidden z-50 flex items-center justify-center">
                <div class="bg-white rounded-2xl w-full max-w-2xl mx-4 shadow-2xl transform scale-95 opacity-0 transition-all duration-300" id="purchaseModalContent">
                    <div class="p-6 border-b border-amber-200 flex justify-between items-center">
                        <h3 class="font-bold text-amber-900 text-xl">采购入库</h3>
                        <button onclick="closePurchaseModal()" class="text-amber-600 hover:text-amber-800">
                            <i class="fas fa-times text-xl"></i>
                        </button>
                    </div>
                    <div class="p-6 space-y-4 max-h-96 overflow-y-auto">
                        <div class="grid grid-cols-2 gap-4">
                            <div>
                                <label class="block text-sm font-medium text-amber-800 mb-2">供应商 <span class="text-red-500">*</span></label>
                                <input type="text" id="purchaseSupplier" class="w-full px-4 py-2 rounded-lg border border-amber-200 focus:outline-none focus:border-amber-500" placeholder="输入供应商名称">
                            </div>
                            <div>
                                <label class="block text-sm font-medium text-amber-800 mb-2">采购日期</label>
                                <input type="date" id="purchaseDate" class="w-full px-4 py-2 rounded-lg border border-amber-200 focus:outline-none focus:border-amber-500">
                            </div>
                        </div>

                        <div class="space-y-3" id="purchaseItems">
                            <div class="flex gap-3 items-end purchase-item">
                                <div class="flex-1">
                                    <label class="block text-sm font-medium text-amber-800 mb-2">原料</label>
                                    <select class="w-full px-4 py-2 rounded-lg border border-amber-200 focus:outline-none focus:border-amber-500 purchase-material">
                                        <option value="">选择原料</option>
                                    </select>
                                </div>
                                <div class="w-24">
                                    <label class="block text-sm font-medium text-amber-800 mb-2">数量</label>
                                    <input type="number" class="w-full px-4 py-2 rounded-lg border border-amber-200 focus:outline-none focus:border-amber-500 purchase-qty" placeholder="0" min="1">
                                </div>
                                <div class="w-32">
                                    <label class="block text-sm font-medium text-amber-800 mb-2">单价</label>
                                    <input type="number" class="w-full px-4 py-2 rounded-lg border border-amber-200 focus:outline-none focus:border-amber-500 purchase-price" placeholder="0.00" min="0" step="0.01">
                                </div>
                                <button onclick="removePurchaseItem(this)" class="mb-2 text-red-500 hover:text-red-700">
                                    <i class="fas fa-trash"></i>
                                </button>
                            </div>
                        </div>

                        <button onclick="addPurchaseItem()" class="w-full py-2 border-2 border-dashed border-amber-300 text-amber-600 rounded-lg hover:bg-amber-50 transition-colors">
                            <i class="fas fa-plus mr-2"></i>添加原料
                        </button>

                        <div class="flex justify-between items-center pt-4 border-t border-amber-200">
                            <span class="text-amber-800 font-medium">合计金额:</span>
                            <span class="text-2xl font-bold text-amber-900" id="purchaseTotal">¥0.00</span>
                        </div>
                    </div>
                    <div class="p-6 border-t border-amber-200 flex gap-3">
                        <button onclick="closePurchaseModal()" class="flex-1 py-2 px-4 rounded-lg border border-amber-300 text-amber-700 hover:bg-amber-50 transition-colors">取消</button>
                        <button onclick="submitPurchase()" class="flex-1 py-2 px-4 rounded-lg btn-primary">确认入库</button>
                    </div>
                </div>
            </div>

            <!-- 订单详情模态框 -->
            <div id="orderDetailModal" class="fixed inset-0 modal-backdrop hidden z-50 flex items-center justify-center">
                <div class="bg-white rounded-2xl w-full max-w-lg mx-4 shadow-2xl transform scale-95 opacity-0 transition-all duration-300" id="orderDetailModalContent">
                    <div class="p-6 border-b border-amber-200 flex justify-between items-center">
                        <h3 class="font-bold text-amber-900 text-xl">订单详情</h3>
                        <button onclick="closeOrderDetailModal()" class="text-amber-600 hover:text-amber-800">
                            <i class="fas fa-times text-xl"></i>
                        </button>
                    </div>
                    <div class="p-6" id="orderDetailContent">
                        <!-- 动态填充订单详情 -->
                    </div>
                </div>
            </div>
            <script src="/js/index.js"></script>
            </body>
            </html>
    )delimiter";
    };
    inline std::string_view HtmlManager::getMainWindowView() {
        return kMainWindowHtml;;
    }
}
#endif //WEBPROJECT_HTMLCONTENT_H
