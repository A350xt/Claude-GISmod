#include "where.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <memory>

namespace gis {

namespace {

const char* GRAMMAR = "pandas query 语法子集：比较 == != < <= > >=、and/or/not（& | ~）、in / not in [..]、"
                      "算术 + - * / % **、`反引号` 包裹列名、时间列可与 ISO 文本比较、"
                      "col.str.contains('x') / col.isna()";

[[noreturn]] void fail(const std::string& where, const std::string& msg) {
    throw GisError(E_BAD_REQUEST, "where 表达式无效:" + msg, json{{"where", where}, {"grammar", GRAMMAR}});
}

// ---------------------------------------------------------------- 词法

enum class Tok { Num, Str, Ident, Op, End };
struct Token {
    Tok kind;
    std::string text;
    double num = 0;
};

std::vector<Token> tokenize(const std::string& src) {
    std::vector<Token> out;
    size_t i = 0;
    auto is_ident_start = [](unsigned char c) { return std::isalpha(c) || c == '_' || c >= 0x80; };
    auto is_ident = [](unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; };
    while (i < src.size()) {
        const unsigned char c = src[i];
        if (std::isspace(c)) {
            ++i;
            continue;
        }
        if (std::isdigit(c) || (c == '.' && i + 1 < src.size() && std::isdigit(static_cast<unsigned char>(src[i + 1])))) {
            size_t j = i;
            while (j < src.size() && (std::isdigit(static_cast<unsigned char>(src[j])) || src[j] == '.' || src[j] == '_')) ++j;
            if (j < src.size() && (src[j] == 'e' || src[j] == 'E')) {
                ++j;
                if (j < src.size() && (src[j] == '+' || src[j] == '-')) ++j;
                while (j < src.size() && std::isdigit(static_cast<unsigned char>(src[j]))) ++j;
            }
            std::string lit = replace_all(src.substr(i, j - i), "_", "");
            char* end = nullptr;
            const double v = std::strtod(lit.c_str(), &end);
            if (end != lit.c_str() + lit.size()) fail(src, "无法识别的数字 " + lit);
            out.push_back({Tok::Num, lit, v});
            i = j;
            continue;
        }
        if (c == '\'' || c == '"') {
            std::string s;
            size_t j = i + 1;
            bool closed = false;
            while (j < src.size()) {
                if (src[j] == '\\' && j + 1 < src.size()) {
                    const char e = src[j + 1];
                    s.push_back(e == 'n' ? '\n' : e == 't' ? '\t' : e);
                    j += 2;
                    continue;
                }
                if (src[j] == static_cast<char>(c)) {
                    closed = true;
                    ++j;
                    break;
                }
                s.push_back(src[j++]);
            }
            if (!closed) fail(src, "字符串没有闭合引号");
            out.push_back({Tok::Str, s});
            i = j;
            continue;
        }
        if (c == '`') {
            const size_t j = src.find('`', i + 1);
            if (j == std::string::npos) fail(src, "反引号没有闭合");
            out.push_back({Tok::Ident, src.substr(i + 1, j - i - 1)});
            i = j + 1;
            continue;
        }
        if (is_ident_start(c)) {
            size_t j = i;
            while (j < src.size() && is_ident(static_cast<unsigned char>(src[j]))) ++j;
            out.push_back({Tok::Ident, src.substr(i, j - i)});
            i = j;
            continue;
        }
        static const char* ops[] = {"**", "==", "!=", "<=", ">=", "<", ">", "+", "-", "*", "/", "%",
                                    "(",  ")",  "[",  "]",  ",", ".", "&", "|", "~"};
        bool matched = false;
        for (const char* op : ops) {
            const size_t n = std::char_traits<char>::length(op);
            if (src.compare(i, n, op) == 0) {
                out.push_back({Tok::Op, op});
                i += n;
                matched = true;
                break;
            }
        }
        if (!matched) fail(src, std::string("无法识别的字符 '") + static_cast<char>(c) + "'");
    }
    out.push_back({Tok::End, ""});
    return out;
}

// ---------------------------------------------------------------- 值

struct V {
    enum Kind { Null, Num, Str, Bool, DateTime, List } kind = Null;
    double n = 0;
    std::string s;
    bool b = false;
    std::vector<V> list;

    static V null() { return {}; }
    static V number(double x) {
        V v;
        v.kind = std::isnan(x) ? Null : Num;
        v.n = x;
        return v;
    }
    static V text(std::string x) {
        V v;
        v.kind = Str;
        v.s = std::move(x);
        return v;
    }
    static V boolean(bool x) {
        V v;
        v.kind = Bool;
        v.b = x;
        return v;
    }
    static V datetime(double x) {
        V v;
        v.kind = std::isnan(x) ? Null : DateTime;
        v.n = x;
        return v;
    }
};

// ---------------------------------------------------------------- 语法树

struct Node;
using NodeP = std::unique_ptr<Node>;
struct Node {
    enum Kind { Lit, Col, Unary, Binary, Compare, Logic, Not, ListLit, Method } kind;
    V lit;
    int col = -1;
    std::string op;
    std::vector<std::string> ops;  // Compare 链
    std::vector<NodeP> kids;
    std::string method;
};

class Parser {
public:
    Parser(const std::string& src, const Layer& layer) : src_(src), toks_(tokenize(src)), layer_(layer) {}

    NodeP parse() {
        NodeP n = parse_or();
        if (peek().kind != Tok::End) fail(src_, "多余的内容：" + peek().text);
        return n;
    }

private:
    const std::string& src_;
    std::vector<Token> toks_;
    size_t pos_ = 0;
    const Layer& layer_;

    const Token& peek(size_t k = 0) const { return toks_[std::min(pos_ + k, toks_.size() - 1)]; }
    bool is_op(const char* op, size_t k = 0) const { return peek(k).kind == Tok::Op && peek(k).text == op; }
    bool is_kw(const char* kw, size_t k = 0) const { return peek(k).kind == Tok::Ident && peek(k).text == kw; }
    void expect(const char* op) {
        if (!is_op(op)) fail(src_, std::string("缺少 '") + op + "'");
        ++pos_;
    }

    static NodeP make(Node::Kind k) {
        auto n = std::make_unique<Node>();
        n->kind = k;
        return n;
    }

    NodeP parse_or() {
        NodeP left = parse_and();
        while (is_kw("or") || is_op("|")) {
            ++pos_;
            NodeP n = make(Node::Logic);
            n->op = "or";
            n->kids.push_back(std::move(left));
            n->kids.push_back(parse_and());
            left = std::move(n);
        }
        return left;
    }

    NodeP parse_and() {
        NodeP left = parse_not();
        while (is_kw("and") || is_op("&")) {
            ++pos_;
            NodeP n = make(Node::Logic);
            n->op = "and";
            n->kids.push_back(std::move(left));
            n->kids.push_back(parse_not());
            left = std::move(n);
        }
        return left;
    }

    NodeP parse_not() {
        if (is_kw("not") || is_op("~")) {
            ++pos_;
            NodeP n = make(Node::Not);
            n->kids.push_back(parse_not());
            return n;
        }
        return parse_compare();
    }

    NodeP parse_compare() {
        NodeP first = parse_add();
        NodeP n;
        auto take_op = [&]() -> std::string {
            for (const char* op : {"==", "!=", "<=", ">=", "<", ">"})
                if (is_op(op)) {
                    ++pos_;
                    return op;
                }
            if (is_kw("in")) {
                ++pos_;
                return "in";
            }
            if (is_kw("not") && is_kw("in", 1)) {
                pos_ += 2;
                return "not in";
            }
            return "";
        };
        for (std::string op = take_op(); !op.empty(); op = take_op()) {
            if (!n) {
                n = make(Node::Compare);
                n->kids.push_back(std::move(first));
            }
            n->ops.push_back(op);
            n->kids.push_back(parse_add());
        }
        return n ? std::move(n) : std::move(first);
    }

    NodeP parse_add() {
        NodeP left = parse_mul();
        while (is_op("+") || is_op("-")) {
            NodeP n = make(Node::Binary);
            n->op = peek().text;
            ++pos_;
            n->kids.push_back(std::move(left));
            n->kids.push_back(parse_mul());
            left = std::move(n);
        }
        return left;
    }

    NodeP parse_mul() {
        NodeP left = parse_unary();
        while (is_op("*") || is_op("/") || is_op("%")) {
            NodeP n = make(Node::Binary);
            n->op = peek().text;
            ++pos_;
            n->kids.push_back(std::move(left));
            n->kids.push_back(parse_unary());
            left = std::move(n);
        }
        return left;
    }

    NodeP parse_unary() {
        if (is_op("-") || is_op("+")) {
            NodeP n = make(Node::Unary);
            n->op = peek().text;
            ++pos_;
            n->kids.push_back(parse_unary());
            return n;
        }
        return parse_power();
    }

    NodeP parse_power() {
        NodeP base = parse_postfix();
        if (is_op("**")) {
            ++pos_;
            NodeP n = make(Node::Binary);
            n->op = "**";
            n->kids.push_back(std::move(base));
            n->kids.push_back(parse_unary());  // 右结合
            return n;
        }
        return base;
    }

    NodeP parse_postfix() {
        NodeP n = parse_primary();
        while (is_op(".")) {
            ++pos_;
            if (peek().kind != Tok::Ident) fail(src_, "'.' 后面需要方法名");
            std::string name = peek().text;
            ++pos_;
            if (name == "str") {
                if (!is_op(".")) fail(src_, ".str 后面需要方法，如 .str.contains('x')");
                ++pos_;
                if (peek().kind != Tok::Ident) fail(src_, ".str 后面需要方法名");
                name = "str." + peek().text;
                ++pos_;
            }
            static const char* known[] = {"str.contains", "str.startswith", "str.endswith", "str.lower", "str.upper",
                                          "isna",         "isnull",         "notna",        "notnull"};
            bool ok = false;
            for (const char* k : known) ok = ok || name == k;
            if (!ok) fail(src_, "不支持的方法 ." + name);
            expect("(");
            NodeP m = make(Node::Method);
            m->method = name;
            m->kids.push_back(std::move(n));
            if (!is_op(")")) {
                m->kids.push_back(parse_or());
                while (is_op(",")) {  // 忽略多余参数（如 contains 的 case=False 不支持）
                    ++pos_;
                    m->kids.push_back(parse_or());
                }
            }
            expect(")");
            n = std::move(m);
        }
        return n;
    }

    NodeP parse_primary() {
        const Token t = peek();
        if (t.kind == Tok::Num) {
            ++pos_;
            NodeP n = make(Node::Lit);
            n->lit = V::number(t.num);
            return n;
        }
        if (t.kind == Tok::Str) {
            ++pos_;
            NodeP n = make(Node::Lit);
            n->lit = V::text(t.text);
            return n;
        }
        if (is_op("(") || is_op("[")) {
            const bool bracket = is_op("[");
            ++pos_;
            const char* close = bracket ? "]" : ")";
            if (is_op(close)) {  // 空列表
                ++pos_;
                return make(Node::ListLit);
            }
            NodeP first = parse_or();
            if (!bracket && is_op(")")) {
                ++pos_;
                return first;  // 普通括号
            }
            NodeP list = make(Node::ListLit);
            list->kids.push_back(std::move(first));
            while (is_op(",")) {
                ++pos_;
                if (is_op(close)) break;
                list->kids.push_back(parse_or());
            }
            expect(close);
            return list;
        }
        if (t.kind == Tok::Ident) {
            ++pos_;
            NodeP n = make(Node::Lit);
            if (t.text == "True") n->lit = V::boolean(true);
            else if (t.text == "False") n->lit = V::boolean(false);
            else if (t.text == "None" || t.text == "nan" || t.text == "NaN") n->lit = V::null();
            else {
                const int idx = layer_.field_index(t.text);
                if (idx < 0) {
                    throw GisError(E_BAD_REQUEST, "where 表达式无效:name '" + t.text + "' is not defined",
                                   json{{"where", src_}, {"columns", layer_.column_names()}, {"grammar", GRAMMAR}});
                }
                n->kind = Node::Col;
                n->col = idx;
            }
            return n;
        }
        fail(src_, t.kind == Tok::End ? "表达式不完整" : "意外的 '" + t.text + "'");
    }
};

// ---------------------------------------------------------------- 求值

class Evaluator {
public:
    Evaluator(const Layer& layer, const std::string& src) : layer_(layer), src_(src) {}

    V eval(const Node& n, size_t row) const {
        switch (n.kind) {
            case Node::Lit: return n.lit;
            case Node::Col: return cell(n.col, row);
            case Node::ListLit: {
                V v;
                v.kind = V::List;
                for (const auto& k : n.kids) v.list.push_back(eval(*k, row));
                return v;
            }
            case Node::Unary: {
                V x = eval(*n.kids[0], row);
                if (x.kind == V::Null) return x;
                if (x.kind != V::Num && x.kind != V::Bool) fail(src_, "一元 " + n.op + " 只能用于数字");
                const double d = x.kind == V::Bool ? (x.b ? 1 : 0) : x.n;
                return V::number(n.op == "-" ? -d : d);
            }
            case Node::Binary: return binary(n.op, eval(*n.kids[0], row), eval(*n.kids[1], row));
            case Node::Compare: {
                V left = eval(*n.kids[0], row);
                for (size_t i = 0; i < n.ops.size(); ++i) {
                    V right = eval(*n.kids[i + 1], row);
                    if (!compare(n.ops[i], left, right)) return V::boolean(false);
                    left = std::move(right);
                }
                return V::boolean(true);
            }
            case Node::Logic: {
                const bool a = truth(eval(*n.kids[0], row));
                if (n.op == "and" && !a) return V::boolean(false);
                if (n.op == "or" && a) return V::boolean(true);
                return V::boolean(truth(eval(*n.kids[1], row)));
            }
            case Node::Not: return V::boolean(!truth(eval(*n.kids[0], row)));
            case Node::Method: return method(n, row);
        }
        return V::null();
    }

    bool truth(const V& v) const {
        switch (v.kind) {
            case V::Bool: return v.b;
            case V::Null: return false;
            case V::Num: return v.n != 0;
            default: fail(src_, "条件结果必须是真/假（是不是少了比较运算符？）");
        }
    }

private:
    const Layer& layer_;
    const std::string& src_;

    V cell(int col, size_t row) const {
        const Value& v = layer_.rows[row][col];
        if (is_null(v)) return V::null();
        switch (layer_.fields[col].type) {
            case FieldType::DateTime: return V::datetime(as_double(v));
            case FieldType::Boolean: return V::boolean(as_double(v) != 0);
            case FieldType::String: return V::text(as_text(v, FieldType::String));
            default: return V::number(as_double(v));
        }
    }

    static double numeric(const V& v) { return v.kind == V::Bool ? (v.b ? 1.0 : 0.0) : v.n; }
    static bool is_numeric(const V& v) { return v.kind == V::Num || v.kind == V::Bool || v.kind == V::DateTime; }

    V binary(const std::string& op, const V& a, const V& b) const {
        if (a.kind == V::Null || b.kind == V::Null) return V::null();
        if (op == "+" && a.kind == V::Str && b.kind == V::Str) return V::text(a.s + b.s);
        if (!is_numeric(a) || !is_numeric(b)) fail(src_, "运算 " + op + " 两侧需要数字");
        const double x = numeric(a), y = numeric(b);
        if (op == "+") return V::number(x + y);
        if (op == "-") return V::number(x - y);
        if (op == "*") return V::number(x * y);
        if (op == "/") return V::number(y == 0 ? NaN : x / y);
        if (op == "%") return V::number(y == 0 ? NaN : x - std::floor(x / y) * y);
        return V::number(std::pow(x, y));
    }

    /// 时间列与文本比较：把文本解析成时间
    V coerce_datetime(const V& v) const {
        if (v.kind != V::Str) return v;
        auto e = parse_datetime(v.s);
        if (!e) fail(src_, "无法把 '" + v.s + "' 解析为时间（支持 ISO 8601，如 2024-09-01T12:00:00）");
        return V::datetime(*e);
    }

    bool equal(V a, V b) const {
        if (a.kind == V::DateTime) b = coerce_datetime(b);
        if (b.kind == V::DateTime) a = coerce_datetime(a);
        if (a.kind == V::Null || b.kind == V::Null) return false;
        if (a.kind == V::Str || b.kind == V::Str) return a.kind == b.kind && a.s == b.s;
        return numeric(a) == numeric(b);
    }

    bool compare(const std::string& op, V a, V b) const {
        if (op == "in" || op == "not in") {
            if (b.kind != V::List) b = [&] {
                V l;
                l.kind = V::List;
                l.list.push_back(b);
                return l;
            }();
            bool hit = false;
            for (const auto& item : b.list) hit = hit || equal(a, item);
            return op == "in" ? hit : !hit;
        }
        if (op == "==") return equal(a, b);
        if (op == "!=") return !equal(a, b);
        if (a.kind == V::DateTime) b = coerce_datetime(b);
        if (b.kind == V::DateTime) a = coerce_datetime(a);
        if (a.kind == V::Null || b.kind == V::Null) return false;
        if (a.kind == V::Str && b.kind == V::Str) {
            const int c = a.s.compare(b.s);
            return op == "<" ? c < 0 : op == "<=" ? c <= 0 : op == ">" ? c > 0 : c >= 0;
        }
        if (a.kind == V::Str || b.kind == V::Str) fail(src_, "不能用 " + op + " 比较文本与数字");
        const double x = numeric(a), y = numeric(b);
        return op == "<" ? x < y : op == "<=" ? x <= y : op == ">" ? x > y : x >= y;
    }

    V method(const Node& n, size_t row) const {
        const V target = eval(*n.kids[0], row);
        const std::string& m = n.method;
        if (m == "isna" || m == "isnull") return V::boolean(target.kind == V::Null);
        if (m == "notna" || m == "notnull") return V::boolean(target.kind != V::Null);
        if (target.kind == V::Null) return m.rfind("str.lower", 0) == 0 || m == "str.upper" ? V::null() : V::boolean(false);
        if (target.kind != V::Str) fail(src_, "." + m + " 只能用于文本列");
        if (m == "str.lower") return V::text(lower(target.s));
        if (m == "str.upper") {
            std::string s = target.s;
            for (auto& ch : s)
                if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 32);
            return V::text(s);
        }
        if (n.kids.size() < 2) fail(src_, "." + m + " 需要一个参数");
        const V arg = eval(*n.kids[1], row);
        if (arg.kind != V::Str) fail(src_, "." + m + " 的参数需要是文本");
        if (m == "str.contains") return V::boolean(target.s.find(arg.s) != std::string::npos);
        if (m == "str.startswith") return V::boolean(target.s.rfind(arg.s, 0) == 0);
        return V::boolean(target.s.size() >= arg.s.size() &&
                          target.s.compare(target.s.size() - arg.s.size(), arg.s.size(), arg.s) == 0);
    }
};

}  // namespace

std::vector<size_t> filter_rows(const Layer& layer, const std::string& where) {
    if (trim(where).empty()) throw GisError(E_BAD_REQUEST, "filter 需要非空 where 表达式");
    Parser parser(where, layer);
    NodeP root = parser.parse();
    Evaluator ev(layer, where);
    std::vector<size_t> out;
    for (size_t r = 0; r < layer.size(); ++r)
        if (ev.truth(ev.eval(*root, r))) out.push_back(r);
    return out;
}

}  // namespace gis
