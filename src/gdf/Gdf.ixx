module;

#include <array>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

export module openape.gdf;

import openape.util;

// A .gdf "definition file" declares the variables of one asset type (xmodel, material, ...)
// and a tree of UI components that edit them.  See ape_work/decomp/NOTES.md for the grammar.
export namespace oa::gdf
{
    enum class VarType { Int, Float, String, Enum, Color, KeyEnum };

    struct EnumItem
    {
        std::vector<std::string> names;   // "a" | "b" -> aliases, first one is the canonical label
        const std::string& Label() const { return names.front(); }
        bool Matches(std::string_view v) const
        {
            for (auto& n : names) if (n == v) return true;
            return false;
        }
    };

    struct Variable
    {
        std::string name;
        VarType type = VarType::String;
        double defNum = 0, minV = 0, maxV = 0;
        std::string defStr;
        std::array<float, 4> defColor{ 1, 1, 1, 1 };
        std::vector<EnumItem> items;
        int defItem = 0;
        std::vector<std::string> keyGdfs;   // keyenum: entries of these types are the allowed values

        int FindItem(std::string_view v) const
        {
            for (size_t i = 0; i < items.size(); ++i) if (items[i].Matches(v)) return (int)i;
            return -1;
        }

        std::string DefaultString() const
        {
            switch (type)
            {
            case VarType::Int:    return std::to_string((long long)std::llround(defNum));
            case VarType::Float:  return FormatFloat(defNum);
            case VarType::Enum:   return items.empty() ? std::string() : items[defItem].Label();
            case VarType::Color:  return FormatFloat(defColor[0]) + " " + FormatFloat(defColor[1]) + " " + FormatFloat(defColor[2]) + " " + FormatFloat(defColor[3]);
            default:              return defStr;
            }
        }
    };

    // Result of evaluating an expression.
    struct Value
    {
        bool isString = false;
        bool isNull = false;
        double num = 0;
        std::string str;
        std::vector<std::string> alts;   // other accepted spellings (enum aliases)

        bool Truthy() const { return isNull ? false : isString ? !str.empty() : num != 0; }
        bool EqualsValue(const Value& o) const
        {
            if (isNull || o.isNull) return isNull && o.isNull;
            if (isString || o.isString)
            {
                auto strip = [](const std::string& s) { return (!s.empty() && s.back() == '*') ? s.substr(0, s.size() - 1) : s; };
                auto spellings = [&](const Value& v) {
                    std::vector<std::string> r{ strip(v.str) };
                    for (auto& a : v.alts) r.push_back(strip(a));
                    return r;
                };
                auto a = spellings(*this), b = spellings(o);
                for (auto& x : a) for (auto& y : b) if (x == y) return true;
                return false;
            }
            return num == o.num;
        }
    };

    class IValueSource
    {
    public:
        virtual ~IValueSource() = default;
        virtual const Variable* FindVariable(std::string_view name) const = 0;
        virtual Value GetValue(const Variable& v) const = 0;
    };

    class Expr
    {
    public:
        virtual ~Expr() = default;
        virtual Value Eval(const IValueSource& src) const = 0;
        // For component bindings: the name of the variable the expression selects ("" for NULL).
        virtual std::string VarName(const IValueSource&) const { return {}; }
    };
    using ExprPtr = std::unique_ptr<Expr>;

    class LiteralExpr : public Expr
    {
    public:
        explicit LiteralExpr(Value v) : m_value(std::move(v)) {}
        Value Eval(const IValueSource&) const override { return m_value; }
        const Value& Get() const { return m_value; }
    private:
        Value m_value;
    };

    class VariableExpr : public Expr
    {
    public:
        explicit VariableExpr(std::string n) : m_name(std::move(n)) {}
        Value Eval(const IValueSource& src) const override
        {
            if (auto* v = src.FindVariable(m_name)) return src.GetValue(*v);
            Value r; r.isNull = true; return r;
        }
        std::string VarName(const IValueSource&) const override { return m_name; }
        const std::string& Name() const { return m_name; }
    private:
        std::string m_name;
    };

    class UnaryExpr : public Expr
    {
    public:
        explicit UnaryExpr(ExprPtr e) : m_e(std::move(e)) {}
        Value Eval(const IValueSource& src) const override { Value r; r.num = m_e->Eval(src).Truthy() ? 0 : 1; return r; }
    private:
        ExprPtr m_e;
    };

    class BinaryExpr : public Expr
    {
    public:
        enum class Op { Eq, Ne, And, Or, Lt, Gt, Le, Ge };
        BinaryExpr(Op op, ExprPtr a, ExprPtr b) : m_op(op), m_a(std::move(a)), m_b(std::move(b)) {}
        Value Eval(const IValueSource& src) const override
        {
            Value r;
            Value a = m_a->Eval(src);
            switch (m_op)
            {
            case Op::And: r.num = (a.Truthy() && m_b->Eval(src).Truthy()) ? 1 : 0; return r;
            case Op::Or:  r.num = (a.Truthy() || m_b->Eval(src).Truthy()) ? 1 : 0; return r;
            default: break;
            }
            Value b = m_b->Eval(src);
            switch (m_op)
            {
            case Op::Eq: r.num = a.EqualsValue(b) ? 1 : 0; break;
            case Op::Ne: r.num = a.EqualsValue(b) ? 0 : 1; break;
            case Op::Lt: r.num = a.num <  b.num; break;
            case Op::Gt: r.num = a.num >  b.num; break;
            case Op::Le: r.num = a.num <= b.num; break;
            case Op::Ge: r.num = a.num >= b.num; break;
            default: break;
            }
            return r;
        }
    private:
        Op m_op;
        ExprPtr m_a, m_b;
    };

    class TernaryExpr : public Expr
    {
    public:
        TernaryExpr(ExprPtr c, ExprPtr a, ExprPtr b) : m_c(std::move(c)), m_a(std::move(a)), m_b(std::move(b)) {}
        Value Eval(const IValueSource& src) const override { return m_c->Eval(src).Truthy() ? m_a->Eval(src) : m_b->Eval(src); }
        std::string VarName(const IValueSource& src) const override
        {
            return m_c->Eval(src).Truthy() ? m_a->VarName(src) : m_b->VarName(src);
        }
    private:
        ExprPtr m_c, m_a, m_b;
    };

    struct Command
    {
        std::string name;
        std::vector<ExprPtr> args;
    };

    enum class CompKind
    {
        HContainer, VContainer, ScrollBox, SimpleListBox, Edit, MultiEdit, SpinEdit, FloatEdit,
        FileEdit, MultiFileEdit, CheckBox, ColorPicker, Button, ListBox, DataView, Unknown
    };

    struct Component
    {
        CompKind kind = CompKind::Unknown;
        std::string typeName;
        std::vector<ExprPtr> args;
        std::vector<Command> commands;
        std::vector<std::unique_ptr<Component>> children;

        // hints resolved from the `exec` block
        int width = -1, height = -1, labelWidth = -1;
        std::string label;
        bool hasLabel = false;
        std::string tooltip;
        const Expr* visible = nullptr;
        bool unsorted = false, rightText = false, leftText = false;
        std::string relDir;
        bool hasGroup = false;
        std::string groupTitle;
        bool groupExpanded = true;

        bool IsContainer() const { return kind == CompKind::HContainer || kind == CompKind::VContainer; }

        std::string BoundVariable(const IValueSource& src) const
        {
            return args.empty() ? std::string() : args[0]->VarName(src);
        }

        double NumArg(size_t i, const IValueSource& src, double fallback) const
        {
            if (i >= args.size()) return fallback;
            Value v = args[i]->Eval(src);
            return v.isString ? fallback : v.num;
        }
    };

    struct DllRef { std::string platform, name; };

    class Def : public IValueSource
    {
    public:
        std::string fileName;       // "xmodel.gdf"
        std::string displayName;    // #name
        std::vector<DllRef> dlls;
        std::string sourceDir, fileExt;
        std::vector<std::string> targetDirs;
        int version = 0;
        std::vector<Variable> vars;
        std::unique_ptr<Component> root;
        std::vector<std::string> errors;

        static std::unique_ptr<Def> Load(const fs::path& path);

        const Variable* Find(std::string_view name) const
        {
            auto it = m_index.find(std::string(name));
            return it == m_index.end() ? nullptr : &vars[it->second];
        }

        // IValueSource: with no entry bound, evaluate to defaults.
        const Variable* FindVariable(std::string_view name) const override { return Find(name); }
        Value GetValue(const Variable& v) const override { return ValueFromString(v, v.DefaultString()); }

        static Value ValueFromString(const Variable& v, const std::string& s)
        {
            Value r;
            switch (v.type)
            {
            case VarType::Int:
            case VarType::Float: r.num = std::strtod(s.c_str(), nullptr); break;
            case VarType::Enum:
            {
                int i = v.FindItem(s);
                r.isString = true;
                if (i < 0) i = v.defItem;
                if (i >= 0 && i < (int)v.items.size()) { r.str = v.items[i].Label(); r.alts = v.items[i].names; }
                break;
            }
            default: r.isString = true; r.str = s; break;
            }
            return r;
        }

        void RebuildIndex()
        {
            m_index.clear();
            for (size_t i = 0; i < vars.size(); ++i) m_index[vars[i].name] = i;
        }

    private:
        std::unordered_map<std::string, size_t> m_index;
    };

    // ---------------------------------------------------------------- parser
    namespace detail
    {
        enum class Tok { End, Ident, Number, String, Punct, Directive };

        struct Token
        {
            Tok kind = Tok::End;
            std::string text;
            int line = 0;
        };

        class Lexer
        {
        public:
            explicit Lexer(std::string src) : m_src(std::move(src)) { Advance(); }

            const Token& Peek() const { return m_tok; }
            Token Next() { Token t = std::move(m_tok); Advance(); return t; }
            bool IsPunct(std::string_view p) const { return m_tok.kind == Tok::Punct && m_tok.text == p; }
            bool IsIdent(std::string_view p) const { return m_tok.kind == Tok::Ident && m_tok.text == p; }
            bool Accept(std::string_view p) { if (IsPunct(p)) { Advance(); return true; } return false; }

        private:
            void SkipSpace()
            {
                for (;;)
                {
                    while (m_pos < m_src.size() && std::isspace((unsigned char)m_src[m_pos])) { if (m_src[m_pos] == '\n') ++m_line; ++m_pos; }
                    if (m_src.compare(m_pos, 2, "//") == 0) { while (m_pos < m_src.size() && m_src[m_pos] != '\n') ++m_pos; continue; }
                    if (m_src.compare(m_pos, 2, "/*") == 0)
                    {
                        m_pos += 2;
                        while (m_pos < m_src.size() && m_src.compare(m_pos, 2, "*/") != 0) { if (m_src[m_pos] == '\n') ++m_line; ++m_pos; }
                        m_pos += 2; continue;
                    }
                    break;
                }
            }

            void Advance()
            {
                SkipSpace();
                m_tok = Token{ Tok::End, {}, m_line };
                if (m_pos >= m_src.size()) return;
                char c = m_src[m_pos];
                if (c == '"')
                {
                    ++m_pos; m_tok.kind = Tok::String;
                    while (m_pos < m_src.size() && m_src[m_pos] != '"')
                    {
                        if (m_src[m_pos] == '\\' && m_pos + 1 < m_src.size()) ++m_pos;
                        m_tok.text += m_src[m_pos++];
                    }
                    ++m_pos;
                }
                else if (c == '#')
                {
                    size_t b = m_pos++;
                    while (m_pos < m_src.size() && (std::isalnum((unsigned char)m_src[m_pos]) || m_src[m_pos] == '_')) ++m_pos;
                    m_tok.kind = Tok::Directive; m_tok.text = m_src.substr(b, m_pos - b);
                }
                else if (std::isdigit((unsigned char)c) || ((c == '-' || c == '.') && m_pos + 1 < m_src.size() &&
                         (std::isdigit((unsigned char)m_src[m_pos + 1]) || m_src[m_pos + 1] == '.')))
                {
                    size_t b = m_pos++;
                    while (m_pos < m_src.size() && (std::isdigit((unsigned char)m_src[m_pos]) || m_src[m_pos] == '.' ||
                           m_src[m_pos] == 'e' || m_src[m_pos] == 'E' ||
                           ((m_src[m_pos] == '-' || m_src[m_pos] == '+') && (m_src[m_pos - 1] == 'e' || m_src[m_pos - 1] == 'E')))) ++m_pos;
                    m_tok.kind = Tok::Number; m_tok.text = m_src.substr(b, m_pos - b);
                }
                else if (std::isalpha((unsigned char)c) || c == '_')
                {
                    size_t b = m_pos++;
                    while (m_pos < m_src.size() && (std::isalnum((unsigned char)m_src[m_pos]) || m_src[m_pos] == '_')) ++m_pos;
                    m_tok.kind = Tok::Ident; m_tok.text = m_src.substr(b, m_pos - b);
                }
                else
                {
                    m_tok.kind = Tok::Punct;
                    static const char* two[] = { "==", "!=", "&&", "||", "<=", ">=" };
                    for (auto t : two) if (m_src.compare(m_pos, 2, t) == 0) { m_tok.text = t; m_pos += 2; return; }
                    m_tok.text = std::string(1, c); ++m_pos;
                }
            }

            std::string m_src;
            size_t m_pos = 0;
            int m_line = 1;
            Token m_tok;
        };

        struct ParseError { std::string msg; int line; };

        class Parser
        {
        public:
            Parser(Def& def, std::string src) : m_def(def), m_lex(std::move(src)) {}

            void Run()
            {
                try { ParseFile(); }
                catch (const ParseError& e) { m_def.errors.push_back("line " + std::to_string(e.line) + ": " + e.msg); }
                m_def.RebuildIndex();
            }

        private:
            [[noreturn]] void Fail(const std::string& m) { throw ParseError{ m, m_lex.Peek().line }; }
            void Expect(std::string_view p) { if (!m_lex.Accept(p)) Fail("expected '" + std::string(p) + "' but found '" + m_lex.Peek().text + "'"); }
            std::string ExpectString() { if (m_lex.Peek().kind != Tok::String) Fail("expected string"); return m_lex.Next().text; }
            std::string ExpectIdent() { if (m_lex.Peek().kind != Tok::Ident) Fail("expected identifier near '" + m_lex.Peek().text + "'"); return m_lex.Next().text; }
            double ExpectNumber() { if (m_lex.Peek().kind != Tok::Number) Fail("expected number near '" + m_lex.Peek().text + "'"); return std::strtod(m_lex.Next().text.c_str(), nullptr); }

            static CompKind KindOf(const std::string& n)
            {
                static const std::unordered_map<std::string, CompKind> k = {
                    { "hcontainer", CompKind::HContainer }, { "vcontainer", CompKind::VContainer },
                    { "scrollbox", CompKind::ScrollBox }, { "simplelistbox", CompKind::SimpleListBox },
                    { "edit", CompKind::Edit }, { "multiedit", CompKind::MultiEdit },
                    { "spinedit", CompKind::SpinEdit }, { "floatedit", CompKind::FloatEdit },
                    { "fileedit", CompKind::FileEdit }, { "multifileedit", CompKind::MultiFileEdit },
                    { "checkbox", CompKind::CheckBox }, { "colorpicker", CompKind::ColorPicker },
                    { "button", CompKind::Button }, { "listbox", CompKind::ListBox }, { "dataview", CompKind::DataView } };
                auto it = k.find(n);
                return it == k.end() ? CompKind::Unknown : it->second;
            }

            void ParseFile()
            {
                while (m_lex.Peek().kind != Tok::End)
                {
                    const Token& t = m_lex.Peek();
                    if (t.kind == Tok::Directive) ParseDirective();
                    else if (t.kind == Tok::Ident && IsTypeKeyword(t.text)) ParseVariable();
                    else if (t.kind == Tok::Ident && KindOf(t.text) != CompKind::Unknown)
                    {
                        auto c = ParseComponent();
                        if (!m_def.root) m_def.root = std::move(c);
                    }
                    else Fail("unexpected '" + t.text + "'");
                }
            }

            static bool IsTypeKeyword(const std::string& s)
            {
                return s == "int" || s == "float" || s == "string" || s == "enum" || s == "color" || s == "keyenum";
            }

            void ParseDirective()
            {
                std::string d = m_lex.Next().text;
                if (d == "#name") m_def.displayName = ExpectString();
                else if (d == "#dll") { DllRef r; r.platform = ExpectString(); r.name = ExpectString(); m_def.dlls.push_back(r); }
                else if (d == "#source_dir") m_def.sourceDir = ExpectString();
                else if (d == "#target_dir") m_def.targetDirs.push_back(ExpectString());
                else if (d == "#file_ext") m_def.fileExt = ExpectString();
                else if (d == "#version") m_def.version = (int)ExpectNumber();
                else Fail("unknown directive " + d);
            }

            void ParseVariable()
            {
                Variable v;
                std::string t = m_lex.Next().text;
                if (t == "int" || t == "float")
                {
                    v.type = t == "int" ? VarType::Int : VarType::Float;
                    v.defNum = ExpectNumber(); v.minV = ExpectNumber(); v.maxV = ExpectNumber();
                }
                else if (t == "string")
                {
                    v.type = VarType::String;
                    if (m_lex.Peek().kind == Tok::String) v.defStr = m_lex.Next().text;
                }
                else if (t == "color")
                {
                    v.type = VarType::Color;
                    for (auto& c : v.defColor) c = (float)ExpectNumber();
                }
                else if (t == "enum")
                {
                    v.type = VarType::Enum;
                    Expect("{");
                    while (!m_lex.IsPunct("}"))
                    {
                        EnumItem item;
                        item.names.push_back(ExpectString());
                        while (m_lex.Accept("|")) item.names.push_back(ExpectString());
                        v.items.push_back(std::move(item));
                        if (!m_lex.Accept(",")) break;
                    }
                    Expect("}");
                    v.defItem = 0;
                    for (size_t i = 0; i < v.items.size(); ++i)
                        if (EndsWith(v.items[i].Label(), "*")) { v.defItem = (int)i; break; }
                }
                else // keyenum("a.gdf", ...)
                {
                    v.type = VarType::KeyEnum;
                    Expect("(");
                    do v.keyGdfs.push_back(ExpectString()); while (m_lex.Accept(","));
                    Expect(")");
                }
                v.name = ExpectIdent();
                m_def.vars.push_back(std::move(v));
                m_def.RebuildIndex();
            }

            std::unique_ptr<Component> ParseComponent()
            {
                auto c = std::make_unique<Component>();
                c->typeName = ExpectIdent();
                c->kind = KindOf(c->typeName);
                if (c->IsContainer())
                {
                    Expect("{");
                    while (!m_lex.IsPunct("}"))
                    {
                        if (m_lex.Peek().kind != Tok::Ident) Fail("expected component near '" + m_lex.Peek().text + "'");
                        c->children.push_back(ParseComponent());
                    }
                    Expect("}");
                }
                else
                {
                    Expect("(");
                    if (!m_lex.IsPunct(")"))
                        do c->args.push_back(ParseExpr()); while (m_lex.Accept(","));
                    Expect(")");
                }
                if (m_lex.Accept("["))
                {
                    while (!m_lex.IsPunct("]"))
                    {
                        std::string kw = ExpectIdent();
                        if (kw == "exec")
                        {
                            Expect("{");
                            while (!m_lex.IsPunct("}")) c->commands.push_back(ParseCommand());
                            Expect("}");
                        }
                        else if (kw == "filter")
                        {
                            Expect("("); int depth = 1;
                            while (depth > 0 && m_lex.Peek().kind != Tok::End) { if (m_lex.IsPunct("(")) ++depth; else if (m_lex.IsPunct(")")) --depth; m_lex.Next(); }
                        }
                        else Fail("unknown component section '" + kw + "'");
                    }
                    Expect("]");
                }
                ResolveHints(*c);
                return c;
            }

            Command ParseCommand()
            {
                Command cmd;
                cmd.name = ExpectIdent();
                Expect("(");
                if (!m_lex.IsPunct(")"))
                    do cmd.args.push_back(ParseExpr()); while (m_lex.Accept(","));
                Expect(")");
                return cmd;
            }

            void ResolveHints(Component& c)
            {
                static const NullSource none;
                for (auto& cmd : c.commands)
                {
                    auto num = [&](size_t i, double fb) {
                        if (i >= cmd.args.size()) return fb;
                        Value v = cmd.args[i]->Eval(none);
                        return v.isString ? fb : v.num;
                    };
                    auto str = [&](size_t i) { return i < cmd.args.size() ? cmd.args[i]->Eval(none).str : std::string(); };
                    const std::string& n = cmd.name;
                    if (n == "width") c.width = (int)num(0, -1);
                    else if (n == "height") c.height = (int)num(0, -1);
                    else if (n == "labelwidth") c.labelWidth = (int)num(0, -1);
                    else if (n == "label") { c.label = str(0); c.hasLabel = true; }
                    else if (n == "tooltip") c.tooltip = str(0);
                    else if (n == "visible" && !cmd.args.empty()) c.visible = cmd.args[0].get();
                    else if (n == "unsorted") c.unsorted = true;
                    else if (n == "righttext") c.rightText = true;
                    else if (n == "lefttext") c.leftText = true;
                    else if (n == "reldir") c.relDir = str(0);
                    else if (n == "groupBox" || n == "groupbox")
                    {
                        c.hasGroup = true; c.groupTitle = str(0);
                        c.groupExpanded = num(1, 1) != 0;
                    }
                }
            }

            // Evaluates literal-only expressions at parse time.
            struct NullSource : IValueSource
            {
                const Variable* FindVariable(std::string_view) const override { return nullptr; }
                Value GetValue(const Variable&) const override { return {}; }
            };

            ExprPtr ParseExpr() { return ParseTernary(); }

            ExprPtr ParseTernary()
            {
                auto c = ParseOr();
                if (m_lex.Accept("?"))
                {
                    auto a = ParseTernary();
                    Expect(":");
                    auto b = ParseTernary();
                    return std::make_unique<TernaryExpr>(std::move(c), std::move(a), std::move(b));
                }
                return c;
            }
            ExprPtr ParseOr()
            {
                auto l = ParseAnd();
                while (m_lex.Accept("||")) l = std::make_unique<BinaryExpr>(BinaryExpr::Op::Or, std::move(l), ParseAnd());
                return l;
            }
            ExprPtr ParseAnd()
            {
                auto l = ParseEq();
                while (m_lex.Accept("&&")) l = std::make_unique<BinaryExpr>(BinaryExpr::Op::And, std::move(l), ParseEq());
                return l;
            }
            ExprPtr ParseEq()
            {
                auto l = ParseRel();
                for (;;)
                {
                    if (m_lex.Accept("==")) l = std::make_unique<BinaryExpr>(BinaryExpr::Op::Eq, std::move(l), ParseRel());
                    else if (m_lex.Accept("!=")) l = std::make_unique<BinaryExpr>(BinaryExpr::Op::Ne, std::move(l), ParseRel());
                    else return l;
                }
            }
            ExprPtr ParseRel()
            {
                auto l = ParseUnary();
                for (;;)
                {
                    if (m_lex.Accept("<=")) l = std::make_unique<BinaryExpr>(BinaryExpr::Op::Le, std::move(l), ParseUnary());
                    else if (m_lex.Accept(">=")) l = std::make_unique<BinaryExpr>(BinaryExpr::Op::Ge, std::move(l), ParseUnary());
                    else if (m_lex.Accept("<")) l = std::make_unique<BinaryExpr>(BinaryExpr::Op::Lt, std::move(l), ParseUnary());
                    else if (m_lex.Accept(">")) l = std::make_unique<BinaryExpr>(BinaryExpr::Op::Gt, std::move(l), ParseUnary());
                    else return l;
                }
            }
            ExprPtr ParseUnary()
            {
                if (m_lex.Accept("!")) return std::make_unique<UnaryExpr>(ParseUnary());
                return ParsePrimary();
            }
            ExprPtr ParsePrimary()
            {
                const Token& t = m_lex.Peek();
                if (t.kind == Tok::Number)
                {
                    Value v; v.num = std::strtod(m_lex.Next().text.c_str(), nullptr);
                    return std::make_unique<LiteralExpr>(std::move(v));
                }
                if (t.kind == Tok::String)
                {
                    Value v; v.isString = true; v.str = m_lex.Next().text;
                    return std::make_unique<LiteralExpr>(std::move(v));
                }
                if (t.kind == Tok::Ident)
                {
                    std::string n = m_lex.Next().text;
                    if (n == "NULL") { Value v; v.isNull = true; return std::make_unique<LiteralExpr>(std::move(v)); }
                    if (!m_def.Find(n)) m_def.errors.push_back("line " + std::to_string(t.line) + ": unknown variable '" + n + "'");
                    return std::make_unique<VariableExpr>(std::move(n));
                }
                if (m_lex.Accept("(")) { auto e = ParseExpr(); Expect(")"); return e; }
                Fail("unexpected '" + t.text + "' in expression");
            }

            Def& m_def;
            Lexer m_lex;
        };
    }

    inline std::unique_ptr<Def> Def::Load(const fs::path& path)
    {
        std::string text;
        if (!ReadTextFile(path, text)) return nullptr;
        auto d = std::make_unique<Def>();
        d->fileName = path.filename().string();
        detail::Parser(*d, std::move(text)).Run();
        if (d->displayName.empty()) d->displayName = path.stem().string();
        return d;
    }
}
