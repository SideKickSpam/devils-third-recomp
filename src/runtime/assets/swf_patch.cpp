#include "swf_patch.h"
#include "../log.h"

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <set>

namespace assets::swf_patch
{
namespace
{
using Bytes = std::vector<uint8_t>;

enum : uint16_t
{
    TagEnd = 0, TagShowFrame = 1, TagDoAction = 12, TagPlaceObject2 = 26, TagRemoveObject2 = 28, TagDefineEditText = 37,
    TagDefineSprite = 39, TagExportAssets = 56, TagDoInitAction = 59, TagPlaceObject3 = 70, TagImportAssets2 = 71,
};

uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t U32(const uint8_t* p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }
void Put16(Bytes& b, uint32_t v) { b.push_back(uint8_t(v)), b.push_back(uint8_t(v >> 8)); }
void Put32(Bytes& b, uint32_t v) { Put16(b, v & 0xFFFF), Put16(b, v >> 16); }
void PutStr(Bytes& b, const std::string& s) { b.insert(b.end(), s.begin(), s.end()), b.push_back(0); }
void Set16(Bytes& b, size_t at, uint32_t v) { b[at] = uint8_t(v), b[at + 1] = uint8_t(v >> 8); }

struct Tag
{
    uint16_t code;
    Bytes body;
};

std::vector<Tag> ParseTags(const uint8_t* p, size_t size)
{
    std::vector<Tag> tags;
    size_t o = 0;
    while (o + 2 <= size)
    {
        const uint16_t codeLen = U16(p + o);
        o += 2;
        size_t len = codeLen & 0x3F;
        if (len == 0x3F)
        {
            if (o + 4 > size)
                break;
            len = U32(p + o);
            o += 4;
        }
        if (o + len > size)
            break;
        tags.push_back({ uint16_t(codeLen >> 6), Bytes(p + o, p + o + len) });
        o += len;
        if (tags.back().code == TagEnd)
            break;
    }
    return tags;
}

void WriteTags(Bytes& out, const std::vector<Tag>& tags)
{
    for (const Tag& t : tags)
    {
        if (t.body.size() < 0x3F)
            Put16(out, uint32_t(t.code) << 6 | uint32_t(t.body.size()));
        else
        {
            Put16(out, uint32_t(t.code) << 6 | 0x3F);
            Put32(out, uint32_t(t.body.size()));
        }
        out.insert(out.end(), t.body.begin(), t.body.end());
    }
}

// ---- bit fields (most significant bit first) ----
struct BitReader
{
    const uint8_t* p;
    size_t bit = 0;
    uint32_t U(int n)
    {
        uint32_t v = 0;
        for (int i = 0; i < n; i++, bit++)
            v = v << 1 | ((p[bit / 8] >> (7 - bit % 8)) & 1);
        return v;
    }
    int32_t S(int n)
    {
        if (!n)
            return 0;
        const uint32_t v = U(n);
        return (v & (1u << (n - 1))) ? int32_t(v | ~((1u << n) - 1)) : int32_t(v);
    }
    size_t Bytes() const { return (bit + 7) / 8; }
};

struct BitWriter
{
    Bytes out;
    size_t bit = 0;
    void U(uint32_t v, int n)
    {
        for (int i = n - 1; i >= 0; i--, bit++)
        {
            if (bit / 8 >= out.size())
                out.push_back(0);
            if ((v >> i) & 1)
                out[bit / 8] |= uint8_t(0x80 >> (bit % 8));
        }
    }
};

int BitsFor(int32_t v)  // signed
{
    int n = 1;
    while (n < 32 && (v < -(1 << (n - 1)) || v >= (1 << (n - 1))))
        n++;
    return n;
}

struct Matrix
{
    bool scale = false, rotate = false;
    int32_t sx = 0, sy = 0, r0 = 0, r1 = 0, tx = 0, ty = 0;  // 16.16 and twips
};

Matrix ReadMatrix(const uint8_t* p, size_t& size)
{
    BitReader b{ p };
    Matrix m;
    if ((m.scale = b.U(1)))
    {
        const int n = int(b.U(5));
        m.sx = b.S(n), m.sy = b.S(n);
    }
    if ((m.rotate = b.U(1)))
    {
        const int n = int(b.U(5));
        m.r0 = b.S(n), m.r1 = b.S(n);
    }
    const int n = int(b.U(5));
    m.tx = b.S(n), m.ty = b.S(n);
    size = b.Bytes();
    return m;
}

Bytes WriteMatrix(const Matrix& m)
{
    BitWriter w;
    w.U(m.scale, 1);
    if (m.scale)
    {
        const int n = std::max(BitsFor(m.sx), BitsFor(m.sy));
        w.U(uint32_t(n), 5), w.U(uint32_t(m.sx), n), w.U(uint32_t(m.sy), n);
    }
    w.U(m.rotate, 1);
    if (m.rotate)
    {
        const int n = std::max(BitsFor(m.r0), BitsFor(m.r1));
        w.U(uint32_t(n), 5), w.U(uint32_t(m.r0), n), w.U(uint32_t(m.r1), n);
    }
    const int n = std::max(BitsFor(m.tx), BitsFor(m.ty));
    w.U(uint32_t(n), 5), w.U(uint32_t(m.tx), n), w.U(uint32_t(m.ty), n);
    return w.out;
}

// ---- PlaceObject2/3: the fields we read or rewrite ----
struct Placement
{
    uint16_t code = 0;
    uint8_t flags = 0, flags2 = 0;
    uint16_t depth = 0;
    std::optional<uint16_t> character;
    std::optional<Matrix> matrix;
    std::string name;
    Bytes beforeName;   // colour transform, ratio (verbatim)
    Bytes afterName;    // clip depth, and for PlaceObject3 filters/blend/cache (verbatim)
    Bytes clipActions;  // verbatim, from the reserved field on
    bool ok = false;
};

bool HasActions(const Placement& p) { return p.flags & 0x80; }

Placement ReadPlacement(const Tag& t)
{
    Placement pl;
    pl.code = t.code;
    const Bytes& b = t.body;
    size_t o = 0;
    if (b.empty())
        return pl;
    pl.flags = b[o++];
    if (t.code == TagPlaceObject3)
    {
        pl.flags2 = b[o++];
        if (pl.flags2 & 0x18)
            return pl;  // class names and images: not used by the menus we patch
    }
    pl.depth = U16(&b[o]), o += 2;
    if (pl.flags & 0x02)
        pl.character = U16(&b[o]), o += 2;
    if (pl.flags & 0x04)
    {
        size_t n;
        pl.matrix = ReadMatrix(&b[o], n);
        o += n;
    }
    const size_t before = o;
    if (pl.flags & 0x08)  // colour transform with alpha
    {
        BitReader r{ &b[o] };
        const bool add = r.U(1), mult = r.U(1);
        const int n = int(r.U(4));
        for (int i = 0; i < (add ? 4 : 0) + (mult ? 4 : 0); i++)
            r.S(n);
        o += r.Bytes();
    }
    if (pl.flags & 0x10)
        o += 2;  // ratio
    pl.beforeName.assign(b.begin() + long(before), b.begin() + long(o));
    if (pl.flags & 0x20)
    {
        const auto end = std::find(b.begin() + long(o), b.end(), 0);
        pl.name.assign(b.begin() + long(o), end);
        o = size_t(end - b.begin()) + 1;
    }
    const size_t after = o;
    if (!(pl.flags & 0x80))  // no clip actions: the rest (clip depth, filters, blend mode, caching) stays as it is
    {
        pl.afterName.assign(b.begin() + long(after), b.end());
        pl.ok = true;
        return pl;
    }
    if (pl.flags & 0x40)
        o += 2;  // clip depth
    if (t.code == TagPlaceObject3 && (pl.flags2 & 0x07))
        return pl;  // filters, blend modes or caching before clip actions: not expected here
    pl.afterName.assign(b.begin() + long(after), b.begin() + long(o));
    if (pl.flags & 0x80)
        pl.clipActions.assign(b.begin() + long(o), b.end());
    pl.ok = true;
    return pl;
}

Tag WritePlacement(const Placement& pl)
{
    Tag t{ pl.code, {} };
    Bytes& b = t.body;
    uint8_t flags = pl.flags & ~uint8_t(0x02 | 0x04 | 0x20 | 0x80);
    if (pl.character)
        flags |= 0x02;
    if (pl.matrix)
        flags |= 0x04;
    if (!pl.name.empty())
        flags |= 0x20;
    if (!pl.clipActions.empty())
        flags |= 0x80;
    b.push_back(flags);
    if (pl.code == TagPlaceObject3)
        b.push_back(pl.flags2);
    Put16(b, pl.depth);
    if (pl.character)
        Put16(b, *pl.character);
    if (pl.matrix)
    {
        const Bytes m = WriteMatrix(*pl.matrix);
        b.insert(b.end(), m.begin(), m.end());
    }
    b.insert(b.end(), pl.beforeName.begin(), pl.beforeName.end());
    if (!pl.name.empty())
        PutStr(b, pl.name);
    b.insert(b.end(), pl.afterName.begin(), pl.afterName.end());
    b.insert(b.end(), pl.clipActions.begin(), pl.clipActions.end());
    return t;
}

bool IsPlacement(const Tag& t) { return t.code == TagPlaceObject2 || t.code == TagPlaceObject3; }

// ---- ActionScript 2 bytecode ----
struct Asm
{
    Bytes code;
    void Op(uint8_t op) { code.push_back(op); }
    void Action(uint8_t op, const Bytes& body)
    {
        code.push_back(op);
        Put16(code, uint32_t(body.size()));
        code.insert(code.end(), body.begin(), body.end());
    }
    void PushStr(const std::string& s)
    {
        Bytes b{ 0 };
        PutStr(b, s);
        Action(0x96, b);
    }
    void PushInt(int32_t v)
    {
        Bytes b{ 7 };
        Put32(b, uint32_t(v));
        Action(0x96, b);
    }
    void GetVariable() { Op(0x1C); }
    void GetMember() { Op(0x4E); }
    void SetMember() { Op(0x4F); }
    void CallMethod() { Op(0x52); }
    void Pop() { Op(0x17); }
    void This() { PushStr("this"), GetVariable(); }
    // flash.external.ExternalInterface.call(method)
    void CallExternal(const std::string& method)
    {
        PushStr(method), PushInt(1);
        PushStr("flash"), GetVariable(), PushStr("external"), GetMember(), PushStr("ExternalInterface"), GetMember();
        PushStr("call"), CallMethod(), Pop();
    }
    // An anonymous function (ActionDefineFunction) with `body`, left on the stack.
    void Function(const Bytes& body)
    {
        Bytes b;
        PutStr(b, "");
        Put16(b, 0);  // no parameters
        Put16(b, uint32_t(body.size()));
        Action(0x9B, b);
        code.insert(code.end(), body.begin(), body.end());
    }
};

// this[method] = function() { ExternalInterface.call(external) }; this.addEventListener(event, this, method)
void Listen(Asm& a, const std::string& event, const std::string& method, const std::string& external)
{
    Asm body;
    body.CallExternal(external);
    a.This(), a.PushStr(method), a.Function(body.code), a.SetMember();
    a.PushStr(method), a.This(), a.PushStr(event), a.PushInt(3), a.This(), a.PushStr("addEventListener"), a.CallMethod(), a.Pop();
}

// Copy of clip actions with `enableInitCallback = true` turned false (the button stays unknown to UnrealScript) and one
// more record, for the load event, running `load`.
Bytes RewriteClipActions(const Bytes& in, const Bytes& load, bool& sawInit)
{
    // reserved (2), all event flags (4), records { flags (4), size (4), [key (1)], actions }, end (4 zero bytes)
    if (in.size() < 10)
        return {};
    Bytes out(in.begin(), in.begin() + 6);
    out[2] |= 0x01;  // ClipEventLoad in the all-events mask
    size_t o = 6;
    while (o + 4 <= in.size())
    {
        const uint32_t flags = U32(&in[o]);
        if (!flags)
            break;
        const uint32_t size = U32(&in[o + 4]);
        if (o + 8 + size > in.size())
            return {};
        Bytes record(in.begin() + long(o + 8), in.begin() + long(o + 8 + size));
        // Find "push <enableInitCallback>, true" (constant pool or literal string) and make it false.
        std::vector<std::string> pool;
        for (size_t a = (flags & 0x00020000) ? 1 : 0; a < record.size();)
        {
            const uint8_t op = record[a];
            if (!op)
                break;
            const size_t len = op >= 0x80 ? U16(&record[a + 1]) : 0, body = a + (op >= 0x80 ? 3 : 1);
            if (op == 0x88)
            {
                pool.clear();
                size_t p = body + 2;
                for (uint16_t i = 0; i < U16(&record[body]) && p < body + len; i++)
                {
                    const auto end = std::find(record.begin() + long(p), record.begin() + long(body + len), 0);
                    pool.emplace_back(record.begin() + long(p), end);
                    p = size_t(end - record.begin()) + 1;
                }
            }
            else if (op == 0x96)
            {
                bool init = false;
                for (size_t p = body; p < body + len;)
                {
                    const uint8_t type = record[p++];
                    if (type == 0)
                    {
                        const auto end = std::find(record.begin() + long(p), record.begin() + long(body + len), 0);
                        init = std::string(record.begin() + long(p), end) == "enableInitCallback";
                        p = size_t(end - record.begin()) + 1;
                    }
                    else if (type == 8 || type == 9)
                    {
                        const uint16_t i = type == 8 ? record[p] : U16(&record[p]);
                        init = i < pool.size() && pool[i] == "enableInitCallback";
                        p += type == 8 ? 1 : 2;
                    }
                    else if (type == 5)
                    {
                        if (init && record[p])
                            record[p] = 0, sawInit = true;
                        p += 1, init = false;
                    }
                    else
                    {
                        static const int sizes[] = { 0, 4, 0, 0, 1, 1, 8, 4 };
                        p += type < 8 ? size_t(sizes[type]) : 1;
                        init = false;
                    }
                }
            }
            a = body + len;
        }
        Put32(out, flags), Put32(out, uint32_t(record.size()));
        out.insert(out.end(), record.begin(), record.end());
        o += 8 + size;
    }
    Bytes loadRecord = load;
    loadRecord.push_back(0);  // ActionEndFlag
    Put32(out, 0x00000001), Put32(out, uint32_t(loadRecord.size()));
    out.insert(out.end(), loadRecord.begin(), loadRecord.end());
    Put32(out, 0);
    return out;
}

// DefineEditText with another character id and initial text.
std::optional<Bytes> CloneEditText(const Bytes& b, uint16_t id, const std::string& text)
{
    if (b.size() < 4)
        return {};
    BitReader r{ &b[2] };
    const int n = int(r.U(5));
    for (int i = 0; i < 4; i++)
        r.S(n);
    size_t o = 2 + r.Bytes();
    if (o + 2 > b.size())
        return {};
    const uint8_t f1 = b[o], f2 = b[o + 1];
    o += 2;
    if (f1 & 0x01)
        o += 2;  // font id
    if (f2 & 0x80)
        o = size_t(std::find(b.begin() + long(o), b.end(), 0) - b.begin()) + 1;  // font class
    if ((f1 & 0x01) || (f2 & 0x80))
        o += 2;  // height
    if (f1 & 0x04)
        o += 4;  // colour
    if (f1 & 0x02)
        o += 2;  // max length
    if (f2 & 0x20)
        o += 9;  // layout
    if (o >= b.size())
        return {};
    Bytes out(b.begin(), b.begin() + long(o));  // up to the variable name, which is dropped (a Label binds "text")
    Set16(out, 0, id);
    out[2 + r.Bytes()] |= 0x80;  // HasText
    PutStr(out, "");
    PutStr(out, text);
    return out;
}

// ActionScript with every string `from` (pushed literals and constant pool entries) replaced by `to`.
Bytes ReplaceString(const Bytes& code, const std::string& from, const std::string& to)
{
    Bytes out;
    for (size_t a = 0; a < code.size();)
    {
        const uint8_t op = code[a];
        if (op < 0x80)
        {
            out.push_back(op), a++;
            if (!op)
                break;
            continue;
        }
        const size_t len = U16(&code[a + 1]), body = a + 3;
        Bytes nb;
        auto str = [&](size_t& p) {
            const auto end = std::find(code.begin() + long(p), code.begin() + long(body + len), 0);
            std::string s(code.begin() + long(p), end);
            p = size_t(end - code.begin()) + 1;
            PutStr(nb, s == from ? to : s);
        };
        if (op == 0x88)
        {
            nb.insert(nb.end(), code.begin() + long(body), code.begin() + long(body + 2));
            for (size_t p = body + 2, i = 0; i < U16(&code[body]); i++)
                str(p);
        }
        else if (op == 0x96)
        {
            static const int sizes[] = { 0, 4, 0, 0, 1, 1, 8, 4, 1, 2 };
            for (size_t p = body; p < body + len;)
            {
                const uint8_t type = code[p++];
                nb.push_back(type);
                if (type == 0)
                    str(p);
                else
                {
                    const size_t n = type < 10 ? size_t(sizes[type]) : 0;
                    nb.insert(nb.end(), code.begin() + long(p), code.begin() + long(p + n));
                    p += n;
                }
            }
        }
        else
            nb.assign(code.begin() + long(body), code.begin() + long(body + len));
        out.push_back(op);
        Put16(out, uint32_t(nb.size()));
        out.insert(out.end(), nb.begin(), nb.end());
        a = body + len;
    }
    return out;
}

const std::set<uint16_t> DefineTags = { 2, 6, 7, 10, 11, 14, 20, 21, 22, 32, 33, 34, 35, 36, 37, 39, 46, 48, 60, 75, 83, 84, 90, 91 };

struct Spec
{
    std::string buttonExport;  // the symbol to clone ("options_mc")
    std::string instance;      // its placed instance next to which ours goes ("options")
    std::string shift;         // an instance moved along by the same step to make room ("endgame"), or empty
    int32_t dx = 0, dy = 0;    // our button's offset from `instance` in twips, when there is no `shift`
    std::string newExport, newInstance, label;
    bool removeOnSecondFrame = false;  // the container's other frames are other layouts (the pause menu's multiplayer)
};

bool AddButton(std::vector<uint8_t>& movie, const Spec& spec)
{
    if (movie.size() < 32 || movie[1] != 'F' || (movie[0] != 'G' && movie[0] != 'F'))
        return false;  // compressed or not a movie
    const size_t rectBits = 5 + 4 * size_t(movie[8] >> 3);
    const size_t start = 8 + (rectBits + 7) / 8 + 4;
    std::vector<Tag> tags = ParseTags(movie.data() + start, movie.size() - start);

    std::map<uint16_t, std::string> exports;
    std::map<uint16_t, size_t> spriteAt, editAt;
    uint16_t maxId = 0;
    for (size_t i = 0; i < tags.size(); i++)
    {
        const Tag& t = tags[i];
        if (DefineTags.count(t.code) && t.body.size() >= 2)
        {
            const uint16_t id = U16(t.body.data());
            maxId = std::max(maxId, id);
            if (t.code == TagDefineSprite)
                spriteAt[id] = i;
            if (t.code == TagDefineEditText)
                editAt[id] = i;
        }
        if (t.code == TagExportAssets && t.body.size() >= 2)
        {
            size_t o = 2;
            for (uint16_t k = 0; k < U16(t.body.data()) && o + 2 < t.body.size(); k++)
            {
                const uint16_t id = U16(&t.body[o]);
                const auto end = std::find(t.body.begin() + long(o + 2), t.body.end(), 0);
                exports[id] = std::string(t.body.begin() + long(o + 2), end);
                o = size_t(end - t.body.begin()) + 1;
            }
        }
        if (t.code == TagImportAssets2)
        {
            size_t o = size_t(std::find(t.body.begin(), t.body.end(), 0) - t.body.begin()) + 1 + 2;
            if (o + 2 > t.body.size())
                continue;
            const uint16_t count = U16(&t.body[o]);
            o += 2;
            for (uint16_t k = 0; k < count && o + 2 < t.body.size(); k++)
            {
                maxId = std::max(maxId, U16(&t.body[o]));
                o = size_t(std::find(t.body.begin() + long(o + 2), t.body.end(), 0) - t.body.begin()) + 1;
            }
        }
    }
    std::optional<uint16_t> button;
    for (auto& [id, n] : exports)
        if (n == spec.buttonExport)
            button = id;
    if (!button || !spriteAt.count(*button))
        return false;
    auto inner = [&](uint16_t sprite) { return ParseTags(tags[spriteAt[sprite]].body.data() + 4, tags[spriteAt[sprite]].body.size() - 4); };

    // The container: the sprite whose first frame places the button's instance.
    uint16_t container = 0;
    for (auto& [id, at] : spriteAt)
        for (const Tag& t : inner(id))
        {
            if (t.code == TagShowFrame)
                break;
            if (IsPlacement(t))
            {
                const Placement p = ReadPlacement(t);
                if (p.ok && p.name == spec.instance && p.character == *button)
                    container = id;
            }
        }
    if (!container)
        return false;

    // Deep copy of the button: every sprite that holds a "textField" (its state labels) is cloned, with the text
    // fields reading `label` and no class (no export) or clip actions, so nothing renames them; the rest is shared.
    std::vector<Tag> added;
    uint16_t next = maxId;
    std::map<uint16_t, bool> holdsText;
    std::function<bool(uint16_t)> HoldsText = [&](uint16_t sprite) {
        if (auto it = holdsText.find(sprite); it != holdsText.end())
            return it->second;
        holdsText[sprite] = false;
        bool found = false;
        for (const Tag& t : inner(sprite))
            if (IsPlacement(t))
            {
                const Placement p = ReadPlacement(t);
                if (p.ok && p.character &&
                    ((p.name == "textField" && editAt.count(*p.character)) || (spriteAt.count(*p.character) && HoldsText(*p.character))))
                    found = true;
            }
        return holdsText[sprite] = found;
    };
    std::map<uint16_t, uint16_t> cloned;
    bool failed = false;
    std::function<std::vector<Tag>(uint16_t)> CloneChildren = [&](uint16_t sprite) {
        std::vector<Tag> children = inner(sprite);
        for (Tag& t : children)
        {
            if (!IsPlacement(t))
                continue;
            Placement p = ReadPlacement(t);
            if (!p.ok || !p.character)
                continue;
            const uint16_t c = *p.character;
            if (p.name == "textField" && editAt.count(c))
            {
                if (!cloned.count(c))
                {
                    auto body = CloneEditText(tags[editAt[c]].body, ++next, spec.label);
                    if (!body)
                        failed = true;
                    else
                        cloned[c] = next, added.push_back({ TagDefineEditText, *body });
                }
            }
            else if (spriteAt.count(c) && HoldsText(c))
            {
                if (!cloned.count(c))
                {
                    std::vector<Tag> grand = CloneChildren(c);
                    Tag s{ TagDefineSprite, {} };
                    Put16(s.body, ++next);
                    Put16(s.body, U16(&tags[spriteAt[c]].body[2]));
                    WriteTags(s.body, grand);
                    cloned[c] = next;
                    added.push_back(std::move(s));
                }
                p.clipActions.clear();
            }
            else
                continue;
            if (cloned.count(c))
                p.character = cloned[c];
            t = WritePlacement(p);
        }
        return children;
    };
    std::vector<Tag> buttonTags = CloneChildren(*button);
    if (failed || cloned.empty())
        return false;
    const uint16_t newButton = ++next;
    {
        Tag sprite{ TagDefineSprite, {} };
        Put16(sprite.body, newButton);
        Put16(sprite.body, U16(&tags[spriteAt[*button]].body[2]));
        WriteTags(sprite.body, buttonTags);
        added.push_back(std::move(sprite));
    }
    {
        Tag exp{ TagExportAssets, {} };
        Put16(exp.body, 1), Put16(exp.body, newButton);
        PutStr(exp.body, spec.newExport);
        added.push_back(std::move(exp));
    }
    // Its class: the source's registration (Object.registerClass(name, class)) with our export name.
    auto sourceInit = std::find_if(tags.begin(), tags.end(),
        [&](const Tag& t) { return t.code == TagDoInitAction && t.body.size() > 2 && U16(t.body.data()) == *button; });
    if (sourceInit == tags.end())
        return false;
    Tag init{ TagDoInitAction, {} };
    Put16(init.body, newButton);
    {
        const Bytes code = ReplaceString(Bytes(sourceInit->body.begin() + 2, sourceInit->body.end()), spec.buttonExport, spec.newExport);
        init.body.insert(init.body.end(), code.begin(), code.end());
    }

    // Our instance in the container's first frame.
    std::vector<Tag> side = inner(container);
    std::optional<size_t> sourceAt, shiftAt, firstFrame;
    std::set<uint16_t> depths;
    for (size_t i = 0; i < side.size(); i++)
    {
        if (side[i].code == TagShowFrame && !firstFrame)
            firstFrame = i;
        if (!IsPlacement(side[i]))
            continue;
        const Placement p = ReadPlacement(side[i]);
        depths.insert(p.depth);
        if (!firstFrame && p.ok && p.name == spec.instance)
            sourceAt = i;
        if (!firstFrame && p.ok && !spec.shift.empty() && p.name == spec.shift)
            shiftAt = i;
    }
    if (!sourceAt || !firstFrame || (!spec.shift.empty() && !shiftAt))
        return false;
    const Placement source = ReadPlacement(side[*sourceAt]);
    if (!source.matrix || !HasActions(source))
        return false;
    uint16_t depth = 25;
    while (depths.count(depth))
        depth++;
    Placement ours = source;
    ours.code = TagPlaceObject2;
    ours.depth = depth;
    ours.character = newButton;
    ours.name = spec.newInstance;
    if (shiftAt)
    {
        Placement shift = ReadPlacement(side[*shiftAt]);
        if (!shift.matrix)
            return false;
        ours.matrix->tx = shift.matrix->tx, ours.matrix->ty = shift.matrix->ty;
        shift.matrix->tx += shift.matrix->tx - source.matrix->tx;
        shift.matrix->ty += shift.matrix->ty - source.matrix->ty;
        side[*shiftAt] = WritePlacement(shift);
    }
    else
        ours.matrix->tx += spec.dx, ours.matrix->ty += spec.dy;
    Asm load;
    Listen(load, "click", "dtPortClick", "DtPortSettings");
    Listen(load, "focusIn", "dtPortFocus", "DtPortFocus");
    bool sawInit = false;
    ours.clipActions = RewriteClipActions(source.clipActions, load.code, sawInit);
    if (ours.clipActions.empty() || !sawInit)
        return false;
    side.insert(side.begin() + long(*firstFrame), WritePlacement(ours));
    if (spec.removeOnSecondFrame && U16(&tags[spriteAt[container]].body[2]) > 1)
    {
        Tag remove{ TagRemoveObject2, {} };
        Put16(remove.body, depth);
        side.insert(side.begin() + long(*firstFrame) + 2, remove);  // the start of the second frame
    }
    Tag& containerTag = tags[spriteAt[container]];
    Bytes body(containerTag.body.begin(), containerTag.body.begin() + 4);
    WriteTags(body, side);
    containerTag.body = std::move(body);

    // The new definitions go before the container's; our class registration right after the source's.
    const size_t containerAt = spriteAt[container];
    tags.insert(tags.begin() + long(containerAt), added.begin(), added.end());
    auto initAt = std::find_if(tags.begin(), tags.end(),
        [&](const Tag& t) { return t.code == TagDoInitAction && t.body.size() >= 2 && U16(t.body.data()) == *button; });
    const auto lastAdded = tags.begin() + long(containerAt + added.size());
    tags.insert(initAt != tags.end() && initAt >= lastAdded ? initAt + 1 : lastAdded, init);

    Bytes out(movie.begin(), movie.begin() + long(start));
    WriteTags(out, tags);
    const uint32_t size = uint32_t(out.size());
    out[4] = uint8_t(size), out[5] = uint8_t(size >> 8), out[6] = uint8_t(size >> 16), out[7] = uint8_t(size >> 24);
    movie = std::move(out);
    return true;
}
} // namespace

bool AddPauseMenuButton(std::vector<uint8_t>& movie, const std::string& label)
{
    Spec s;
    s.buttonExport = "options_mc", s.instance = "options", s.shift = "endgame";
    s.newExport = "pcsettings_mc", s.newInstance = "pcsettings", s.label = label;
    s.removeOnSecondFrame = true;
    return AddButton(movie, s);
}

bool AddTitleMenuButton(std::vector<uint8_t>& movie, const std::string& label)
{
    Spec s;
    s.buttonExport = "MenuItem_title_Option", s.instance = "Option";
    s.dx = 200 * 20;  // the next tile: the title's tiles stand 200 pixels apart
    s.newExport = "MenuItem_title_dtport", s.newInstance = "PcSettings", s.label = label;
    return AddButton(movie, s);
}

// ---- button prompts: variants of the Wii U button art, chosen at run time ----
namespace
{
// ActionScript with forward labels: If/Jump to a label placed later (or earlier).
struct Script : Asm
{
    std::map<int, size_t> labels;
    std::vector<std::pair<size_t, int>> fixups;  // offset of the 16-bit branch field, label
    int nextLabel = 1000;
    int NewLabel() { return nextLabel++; }  // (each label its own: several are often made before any is placed)
    void Place(int label) { labels[label] = code.size(); }
    void Branch(uint8_t op, int label)
    {
        code.push_back(op), Put16(code, 2), Put16(code, 0);
        fixups.push_back({ code.size() - 2, label });
    }
    void If(int label) { Branch(0x9D, label); }
    void Jump(int label) { Branch(0x99, label); }
    void Resolve()
    {
        for (auto [at, label] : fixups)
        {
            const int32_t off = int32_t(labels.at(label)) - int32_t(at + 2);
            code[at] = uint8_t(off), code[at + 1] = uint8_t(off >> 8);
        }
    }
    void Global(const std::string& prefix)  // _global[prefix + this.dtB]
    {
        PushStr("_global"), GetVariable(), PushStr(prefix), This(), PushStr("dtB"), GetMember(), Op(0x47), GetMember();
    }
    void Kind() { This(), PushStr("dtK"), GetMember(); }
    void SetChild(const std::string& child, const std::string& prop, const std::function<void()>& value)
    {
        This(), PushStr(child), GetMember(), PushStr(prop), value(), SetMember();
    }
};

// Vector art: polygons filled with one solid colour (DefineShape3). Points in pixels.
struct Poly
{
    std::vector<std::pair<float, float>> pts;
    bool hole = false;
};
std::vector<std::pair<float, float>> RoundRect(float w, float h, float r, float inset = 0)
{
    std::vector<std::pair<float, float>> p;
    w -= 2 * inset, h -= 2 * inset, r = std::max(0.0f, r - inset);
    const float x0 = -w / 2 + r, x1 = w / 2 - r, y0 = -h / 2 + r, y1 = h / 2 - r;
    const float cx[4] = { x1, x1, x0, x0 }, cy[4] = { y0, y1, y1, y0 };
    for (int c = 0; c < 4; c++)
        for (int i = 0; i <= 6; i++)
        {
            const float a = (float(c) * 90.0f - 90.0f + float(i) * 15.0f) * 3.14159265f / 180.0f;
            p.push_back({ cx[c] + r * std::cos(a), cy[c] + r * std::sin(a) });
        }
    return p;
}
std::vector<std::pair<float, float>> Circle(float r, float cx = 0, float cy = 0)
{
    std::vector<std::pair<float, float>> p;
    for (int i = 0; i < 40; i++)
    {
        const float a = float(i) * 2 * 3.14159265f / 40;
        p.push_back({ cx + r * std::cos(a), cy + r * std::sin(a) });
    }
    return p;
}
std::vector<std::pair<float, float>> Regular(int n, float r, float rot)
{
    std::vector<std::pair<float, float>> p;
    for (int i = 0; i < n; i++)
    {
        const float a = (rot + float(i) * 360.0f / float(n)) * 3.14159265f / 180.0f;
        p.push_back({ r * std::cos(a), r * std::sin(a) });
    }
    return p;
}

Bytes Shape(uint16_t id, uint32_t rgba, const std::vector<Poly>& polys)
{
    // Orient outer contours clockwise on screen and holes the other way, all filled on their right (fill style 1).
    std::vector<std::vector<std::pair<int32_t, int32_t>>> contours;
    int32_t minX = INT32_MAX, minY = INT32_MAX, maxX = INT32_MIN, maxY = INT32_MIN;
    for (const Poly& p : polys)
    {
        std::vector<std::pair<int32_t, int32_t>> c;
        for (auto [x, y] : p.pts)
        {
            c.push_back({ int32_t(std::lround(x * 20)), int32_t(std::lround(y * 20)) });
            minX = std::min(minX, c.back().first), maxX = std::max(maxX, c.back().first);
            minY = std::min(minY, c.back().second), maxY = std::max(maxY, c.back().second);
        }
        double area = 0;
        for (size_t i = 0; i < c.size(); i++)
        {
            const auto& a = c[i];
            const auto& b = c[(i + 1) % c.size()];
            area += double(a.first) * b.second - double(b.first) * a.second;
        }
        if ((area > 0) == p.hole)
            std::reverse(c.begin(), c.end());
        contours.push_back(std::move(c));
    }
    Bytes out;
    Put16(out, id);
    {
        BitWriter w;
        const int n = std::max({ BitsFor(minX), BitsFor(maxX), BitsFor(minY), BitsFor(maxY) });
        w.U(uint32_t(n), 5), w.U(uint32_t(minX), n), w.U(uint32_t(maxX), n), w.U(uint32_t(minY), n), w.U(uint32_t(maxY), n);
        out.insert(out.end(), w.out.begin(), w.out.end());
    }
    out.push_back(1);  // one fill style: solid RGBA
    out.push_back(0x00), out.push_back(uint8_t(rgba >> 24)), out.push_back(uint8_t(rgba >> 16)), out.push_back(uint8_t(rgba >> 8)),
        out.push_back(uint8_t(rgba));
    out.push_back(0);  // no line styles
    BitWriter w;
    w.U(1, 4), w.U(0, 4);  // fill bits, line bits
    for (const auto& c : contours)
    {
        // style change: move to the first point, fill style 1 = 1, fill style 0 = 0
        w.U(0, 1), w.U(0, 1), w.U(0, 1), w.U(1, 1), w.U(1, 1), w.U(1, 1);
        const int n = std::max(BitsFor(c[0].first), BitsFor(c[0].second));
        w.U(uint32_t(n), 5), w.U(uint32_t(c[0].first), n), w.U(uint32_t(c[0].second), n);
        w.U(0, 1), w.U(1, 1);  // fill style 0 = none, fill style 1 (the right of each edge: inside, clockwise) = 1
        for (size_t i = 1; i <= c.size(); i++)
        {
            const auto& a = c[i - 1];
            const auto& b = c[i % c.size()];
            const int32_t dx = b.first - a.first, dy = b.second - a.second;
            const int bits = std::max({ BitsFor(dx), BitsFor(dy), 2 });
            w.U(1, 1), w.U(1, 1), w.U(uint32_t(bits - 2), 4), w.U(1, 1);  // straight, general line
            w.U(uint32_t(dx), bits), w.U(uint32_t(dy), bits);
        }
    }
    w.U(0, 6);  // end of shape
    out.insert(out.end(), w.out.begin(), w.out.end());
    return out;
}

Tag SpriteTag(uint16_t id, const std::vector<Tag>& frame)
{
    Tag t{ TagDefineSprite, {} };
    Put16(t.body, id), Put16(t.body, 1);
    std::vector<Tag> tags = frame;
    tags.push_back({ TagShowFrame, {} });
    tags.push_back({ TagEnd, {} });
    WriteTags(t.body, tags);
    return t;
}

Tag Place(uint16_t depth, uint16_t character, const std::string& name, float x = 0, float y = 0, bool visible = true)
{
    Placement p;
    p.code = TagPlaceObject2, p.depth = depth, p.character = character, p.name = name;
    Matrix m;
    m.tx = int32_t(std::lround(x * 20)), m.ty = int32_t(std::lround(y * 20));
    p.matrix = m;
    (void)visible;
    p.ok = true;
    return WritePlacement(p);
}

// A text field in the movie's game font: black, centred, no text until the script sets it.
Bytes TextField(uint16_t id, uint16_t font, float width, float height, float size)
{
    Bytes b;
    Put16(b, id);
    BitWriter w;
    const int32_t x0 = int32_t(-width * 10), x1 = int32_t(width * 10), y0 = int32_t(-height * 10), y1 = int32_t(height * 10);
    const int n = std::max({ BitsFor(x0), BitsFor(x1), BitsFor(y0), BitsFor(y1) });
    w.U(uint32_t(n), 5), w.U(uint32_t(x0), n), w.U(uint32_t(x1), n), w.U(uint32_t(y0), n), w.U(uint32_t(y1), n);
    b.insert(b.end(), w.out.begin(), w.out.end());
    b.push_back(0x80 | 0x08 | 0x04 | 0x01);  // HasText, ReadOnly, HasTextColor, HasFont
    b.push_back(0x20 | 0x10 | 0x02 | 0x01);  // HasLayout, NoSelect, HTML (the game's fonts by face), UseOutlines
    Put16(b, font), Put16(b, uint32_t(size * 20));
    b.push_back(10), b.push_back(10), b.push_back(10), b.push_back(255);  // colour
    b.push_back(2);  // align centre
    Put16(b, 0), Put16(b, 0), Put16(b, 0), Put16(b, 0);  // margins, indent, leading
    PutStr(b, "");  // variable
    PutStr(b, "");  // text
    return b;
}

// The Wii U art shapes of the prompt library and the GamePad button each shows.
const std::map<std::string, std::pair<std::string, std::string>> PromptContainers = {
    // container export: art from its first frame, button name used by the run-time variables
    { "AButtonContainer", { "", "A" } }, { "AButtonContainer_6", { "", "A" } }, { "AButtonContainer_E", { "", "A" } },
    { "AButtonContainer_thin", { "", "A" } }, { "BButtonContainer", { "", "B" } }, { "BButtonContainer_space", { "", "B" } },
    { "XButtonContainer", { "", "X" } }, { "YButtonContainer", { "", "Y" } }, { "StartContainer", { "", "Plus" } },
    { "BackContainer", { "", "Minus" } }, { "LBButtonContainer", { "", "L" } }, { "RBButtonContainer", { "", "R" } },
    { "RBButtonContainer_X", { "", "R" } }, { "ButtonContainer_ZoomIn", { "", "L" } }, { "ButtonContainer_MeleeAiming", { "", "L" } },
    { "LTButtonContainer", { "", "ZL" } }, { "RTButtonContainer", { "", "ZR" } }, { "ButtonContainer_GunAiming", { "", "ZL" } },
    { "ButtonContainer_Guard", { "", "ZL" } },
    // Sticks and the D-pad: their own art on controllers, keys on the keyboard.
    { "Dpad_Up", { "", "Up" } }, { "Dpad_Down", { "", "Down" } }, { "Dpad_Left", { "", "Left" } }, { "Dpad_Right", { "", "Right" } },
    { "Dpad", { "", "Dpad" } }, { "WiiUPad_gyro", { "", "Gyro" } }, { "Thumbstick_L", { "", "LClick" } }, { "Thumbstick_L_stop", { "", "LClick" } },
    { "Thumbstick_R", { "", "RClick" } }, { "Thumbstick_R_stop", { "", "RClick" } }, { "Analogstick_L", { "", "LStick" } },
    { "Analogstick_L_move", { "", "LStick" } }, { "Analogstick_L_top_bottom", { "", "LStick" } }, { "Analogstick_R", { "", "RStick" } },
    { "Analogstick_R_move", { "", "RStick" } }, { "Analogstick_R_top_bottom", { "", "RStick" } },
    // "Move any stick" (the throw escape's wiggle).
    { "Analogstick_ANY_move", { "", "Mash" } },
    // "A ✓" boxes (the mission score screen's "go on"): one shape holds the A and the check mark.
    { "guide_go", { "", "A" } }, { "guide_go_white", { "", "A" } },
};

// Containers whose art is not their first picture, or shares a shape with something that must stay.
struct ArtSpec
{
    uint16_t depth;  // the art's depth
    bool withCheck;  // the art is "A ✓" in one shape: variants on its left square, a drawn check while they show
};
const std::map<std::string, ArtSpec> ArtSpecs = { { "guide_go", { 4, true } }, { "guide_go_white", { 4, true } } };

} // namespace

bool AddPromptVariants(std::vector<uint8_t>& movie)
{
    if (movie.size() < 32 || movie[1] != 'F' || (movie[0] != 'G' && movie[0] != 'F'))
        return false;
    const size_t rectBits = 5 + 4 * size_t(movie[8] >> 3);
    const size_t start = 8 + (rectBits + 7) / 8 + 4;
    std::vector<Tag> tags = ParseTags(movie.data() + start, movie.size() - start);

    std::map<uint16_t, std::string> exports;
    std::map<uint16_t, size_t> spriteAt;
    std::map<uint16_t, std::pair<float, float>> shapeSize;
    std::map<uint16_t, std::pair<float, float>> shapeCentre;  // the art's bounding box centre (variants sit on it)  // width, height of shapes (pixels)
    uint16_t maxId = 0, font = 0;
    for (size_t i = 0; i < tags.size(); i++)
    {
        const Tag& t = tags[i];
        if (DefineTags.count(t.code) && t.body.size() >= 2)
        {
            const uint16_t id = U16(t.body.data());
            maxId = std::max(maxId, id);
            if (t.code == TagDefineSprite)
                spriteAt[id] = i;
            if (t.code == 2 || t.code == 22 || t.code == 32 || t.code == 83)
            {
                BitReader r{ &t.body[2] };
                const int n = int(r.U(5));
                const float x0 = float(r.S(n)) / 20, x1 = float(r.S(n)) / 20, y0 = float(r.S(n)) / 20, y1 = float(r.S(n)) / 20;
                shapeSize[id] = { x1 - x0, y1 - y0 };
                shapeCentre[id] = { (x0 + x1) / 2, (y0 + y1) / 2 };
            }
        }
        if (t.code == TagExportAssets && t.body.size() >= 2)
        {
            size_t o = 2;
            for (uint16_t k = 0; k < U16(t.body.data()) && o + 2 < t.body.size(); k++)
            {
                const uint16_t id = U16(&t.body[o]);
                const auto end = std::find(t.body.begin() + long(o + 2), t.body.end(), 0);
                exports[id] = std::string(t.body.begin() + long(o + 2), end);
                o = size_t(end - t.body.begin()) + 1;
            }
        }
        if (t.code == TagImportAssets2)
        {
            size_t o = size_t(std::find(t.body.begin(), t.body.end(), 0) - t.body.begin()) + 1 + 2;
            if (o + 2 > t.body.size())
                continue;
            const uint16_t count = U16(&t.body[o]);
            o += 2;
            for (uint16_t k = 0; k < count && o + 2 < t.body.size(); k++)
            {
                const uint16_t id = U16(&t.body[o]);
                const auto end = std::find(t.body.begin() + long(o + 2), t.body.end(), 0);
                if (std::string(t.body.begin() + long(o + 2), end) == "$NormalFont_en")
                    font = id;
                maxId = std::max(maxId, id);
                o = size_t(end - t.body.begin()) + 1;
            }
        }
    }
    if (!font)
        return false;
    auto inner = [&](uint16_t sprite) { return ParseTags(tags[spriteAt[sprite]].body.data() + 4, tags[spriteAt[sprite]].body.size() - 4); };

    // A picture's box in its own coordinates: a shape's bounds, or the union of what a sprite shows on its first frame.
    struct Box { float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f; bool Empty() const { return x0 > x1; } };
    std::map<uint16_t, Box> boxes;
    std::function<Box(uint16_t, int)> boxOf = [&](uint16_t c, int level) -> Box {
        if (auto it = boxes.find(c); it != boxes.end())
            return it->second;
        Box b;
        if (shapeCentre.count(c))
        {
            const auto [cx, cy] = shapeCentre[c];
            const auto [w, h] = shapeSize[c];
            b = { cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2 };
        }
        else if (spriteAt.count(c) && level < 8)
            for (const Tag& t : inner(c))
            {
                if (t.code == 1)
                    break;
                if (!IsPlacement(t))
                    continue;
                const Placement p = ReadPlacement(t);
                if (!p.ok || !p.character || p.name.size())
                    continue;
                const Box cb = boxOf(*p.character, level + 1);
                if (cb.Empty())
                    continue;
                const Matrix m = p.matrix.value_or(Matrix{});
                const float sx = m.scale ? float(m.sx) / 65536 : 1, sy = m.scale ? float(m.sy) / 65536 : 1;
                const float r0 = m.rotate ? float(m.r0) / 65536 : 0, r1 = m.rotate ? float(m.r1) / 65536 : 0;
                for (float x : { cb.x0, cb.x1 })
                    for (float y : { cb.y0, cb.y1 })
                    {
                        const float X = x * sx + y * r1 + float(m.tx) / 20, Y = x * r0 + y * sy + float(m.ty) / 20;
                        b.x0 = std::min(b.x0, X), b.x1 = std::max(b.x1, X), b.y0 = std::min(b.y0, Y), b.y1 = std::max(b.y1, Y);
                    }
            }
        return boxes[c] = b;
    };

    // The art: every picture a container places at the depth of its first one (the stick-click containers swap in a
    // pressed stick there during their push animation), with the container's button. Keyed by both: containers of
    // different buttons share pictures (Thumbstick_L and Analogstick_L place the same stick).
    std::set<std::pair<uint16_t, std::string>> art;
    std::set<uint16_t> firstArt;  // each container's first picture (other sprites that place it get a wrapper too)
    std::map<uint16_t, uint16_t> artDepthOf;  // container -> the depth its art sits at
    std::set<uint16_t> checkArt, keepOthers;  // "A ✓" art; containers whose other pictures stay (ArtSpecs)
    for (auto& [id, name] : exports)
    {
        auto it = PromptContainers.find(name);
        if (it == PromptContainers.end() || !spriteAt.count(id))
            continue;
        std::optional<uint16_t> artDepth;
        const auto spec = ArtSpecs.find(name);
        if (spec != ArtSpecs.end())
            artDepth = spec->second.depth;
        for (const Tag& t : inner(id))
        {
            if (!IsPlacement(t))
                continue;
            const Placement p = ReadPlacement(t);
            if (!p.ok || !p.character || !(shapeSize.count(*p.character) || spriteAt.count(*p.character)))
                continue;
            if (!artDepth)
                artDepth = p.depth, firstArt.insert(*p.character), artDepthOf[id] = p.depth;
            if (p.depth == *artDepth)
            {
                art.insert({ *p.character, it->second.second });
                artDepthOf[id] = p.depth;
                if (spec != ArtSpecs.end() && spec->second.withCheck)
                    checkArt.insert(*p.character), keepOthers.insert(id);
            }
        }
    }
    if (art.empty())
        return false;

    // The face buttons' art by letter, so a face button's prompt can show another letter's own art (the button bound to
    // it on an Xbox-style or Nintendo controller) instead of a drawn icon.
    static const char* const Faces[] = { "A", "B", "X", "Y" };
    std::map<std::string, std::vector<uint16_t>> faceArt;
    for (auto& [shape, button] : art)
        if (std::find(faceArt[button].begin(), faceArt[button].end(), shape) == faceArt[button].end())
            faceArt[button].push_back(shape);
    auto sizeOf = [&](uint16_t c) { return shapeSize.count(c) ? shapeSize[c] : std::pair<float, float>{ 40.0f, 40.0f }; };

    std::vector<Tag> added;
    uint16_t next = maxId;
    constexpr uint32_t Black = 0x101010FF, White = 0xFFFFFFFF;
    std::map<std::pair<uint16_t, std::string>, uint16_t> wrapper;  // (art, button) -> its wrapper sprite
    for (auto& [shape, button] : art)
    {
        const bool withCheck = checkArt.count(shape) > 0;
        auto [w, h] = shapeSize.count(shape) ? shapeSize[shape] : std::pair<float, float>{ 40.0f, 40.0f };
        if (withCheck)
            h *= 0.85f, w = h;  // the button is the left square of the art (a little smaller: room for the check)
        // Outline style (the "A ✓" boxes): the movies paint that art in one flat tint (the Wii U A is a cut-out shape), so
        // a white key with dark letters became one pale blob. Light outlines and light letters survive it, as the
        // original art does.
        const bool outline = withCheck;
        const uint32_t Ink = outline ? 0xFFFFFFFF : 0x101010FF;
        const float r = std::min(w, h) / 2;
        auto shapeTag = [&](uint32_t colour, const std::vector<Poly>& polys) {
            added.push_back({ 32, Shape(++next, colour, polys) });
            return next;
        };
        // Cap: as tall as the art it replaces (menus frame the art's outline), rounded like a key; face: a circle.
        // Black edge, white body.
        const uint16_t capEdge = outline
            ? shapeTag(White, { { RoundRect(std::max(w, h * 1.15f), h, h * 0.28f) }, { RoundRect(std::max(w, h * 1.15f), h, h * 0.28f, 2.0f), true } })
            : shapeTag(Black, { { RoundRect(std::max(w, h * 1.15f), h, h * 0.28f) } });
        const uint16_t capBody = shapeTag(outline ? 0xFFFFFF00 : White, { { RoundRect(std::max(w, h * 1.15f), h, h * 0.28f, 2.5f) } });
        const uint16_t faceEdge = outline ? shapeTag(White, { { Circle(r) }, { Circle(r - 2.0f), true } }) : shapeTag(Black, { { Circle(r) } });
        const uint16_t faceBody = shapeTag(outline ? 0xFFFFFF00 : White, { { Circle(r - 2.5f) } });
        const float k = r * 0.5f, t = r * 0.11f;  // PlayStation symbols: size, stroke
        std::vector<Poly> cross;
        {
            Poly p;
            for (int i = 0; i < 4; i++)
            {
                const float a = (45.0f + 90.0f * float(i)) * 3.14159265f / 180.0f, ca = std::cos(a), sa = std::sin(a);
                // each arm: two points on its sides at the tip, then the inner corner
                auto rot = [&](float x, float y) { return std::pair<float, float>{ x * ca - y * sa, x * sa + y * ca }; };
                p.pts.push_back(rot(t, -t)), p.pts.push_back(rot(k * 1.25f, -t)), p.pts.push_back(rot(k * 1.25f, t));
            }
            cross.push_back(p);
        }
        const uint16_t symCross = shapeTag(Ink, cross);
        const uint16_t symCircle = shapeTag(Ink, { { Circle(k * 1.1f) }, { Circle(k * 1.1f - t * 1.6f), true } });
        const uint16_t symSquare = shapeTag(Ink, { { RoundRect(k * 2, k * 2, 1) }, { RoundRect(k * 2 - t * 3.2f, k * 2 - t * 3.2f, 0.5f), true } });
        const uint16_t symTriangle = shapeTag(Ink, { { Regular(3, k * 1.25f, -90) }, { Regular(3, k * 1.25f - t * 3.2f, -90), true } });
        // Mouse: a white body with its button split and wheel drawn in black, as a key cap is a white body with a black
        // edge; the button that is meant is filled black.
        const float mw = h * 0.62f, mh = h * 0.9f, mr = mw * 0.45f, line = std::max(1.2f, h * 0.035f);
        auto rect = [](float x0, float y0, float x1, float y1) { return Poly{ { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } } }; };
        auto offset = [](std::vector<std::pair<float, float>> pts, float dx, float dy) {
            for (auto& [x, y] : pts)
                x += dx, y += dy;
            return pts;
        };
        const float top = -mh / 2 + 2, mid = -mh / 2 + mh * 0.44f, side = mw / 2 - 2, ir = mr - 2;
        // The inner body's top edge at x (round at the corners).
        auto topAt = [&](float x) {
            const float c = side - ir;
            if (std::abs(x) <= c)
                return top;
            const float dx = std::abs(x) - c;
            return top + ir - std::sqrt(std::max(0.0f, ir * ir - dx * dx));
        };
        // A button's area (screen-clockwise: along the top, down the split, back along the line).
        auto buttonArea = [&](bool left) {
            Poly p;
            const float xa = left ? -side : line / 2, xb = left ? -line / 2 : side;
            for (int i = 0; i <= 12; i++)
            {
                const float x = xa + (xb - xa) * float(i) / 12;
                p.pts.push_back({ x, topAt(x) });
            }
            p.pts.push_back({ xb, mid }), p.pts.push_back({ xa, mid });
            return p;
        };
        const float ww = mw * 0.2f, wh = mh * 0.2f, wy = top + (mid - top) * 0.55f;
        const uint16_t mouseEdge = shapeTag(Black, { { RoundRect(mw, mh, mr) } });
        const uint16_t mouseBody = shapeTag(White, { { RoundRect(mw, mh, mr, 2) } });
        const uint16_t mouseLines = shapeTag(Black, { rect(-line / 2, top, line / 2, mid), rect(-side, mid - line / 2, side, mid + line / 2) });
        const uint16_t wheelEdge = shapeTag(Black, { { offset(RoundRect(ww, wh, ww / 2), 0, wy) } });
        const uint16_t wheelBody = shapeTag(White, { { offset(RoundRect(ww, wh, ww / 2, line), 0, wy) } });
        const uint16_t mouseLeft = shapeTag(Black, { buttonArea(true) });
        const uint16_t mouseRight = shapeTag(Black, { buttonArea(false) });
        const uint16_t wheelUp = shapeTag(Black, { { { { 0, -mh / 2 - 5 }, { 5, -mh / 2 - 1 }, { -5, -mh / 2 - 1 } } } });
        const uint16_t wheelDown = shapeTag(Black, { { { { 0, mh / 2 + 5 }, { -5, mh / 2 + 1 }, { 5, mh / 2 + 1 } } } });
        const uint16_t text = ++next;
        const float textSize = std::max(10.0f, h * 0.48f);
        // (A text field draws from its top: centred on the picture, as tall as one line.)
        // (Wide enough never to clip a label: the cap is sized to the text at run time, _global.dtW_.)
        added.push_back({ TagDefineEditText, TextField(text, font, 400, textSize * 1.3f, textSize) });

        // Grouped variants: cap, face (with letter), each PlayStation symbol on a face, each mouse picture.
        auto group = [&](std::vector<std::pair<uint16_t, std::string>> parts) {
            std::vector<Tag> frame;
            uint16_t d = 1;
            for (auto& [c, n] : parts)
                frame.push_back(Place(d++, c, n));
            added.push_back(SpriteTag(++next, frame));
            return next;
        };
        const uint16_t cap = group({ { capEdge, "" }, { capBody, "" } });
        const uint16_t face = group({ { faceEdge, "" }, { faceBody, "" } });
        std::vector<uint16_t> syms, mice;
        for (uint16_t s : { symCross, symCircle, symSquare, symTriangle })
            syms.push_back(group({ { faceEdge, "" }, { faceBody, "" }, { s, "" } }));
        // Left, middle (the wheel filled), right, wheel up and down (filled, with an arrow).
        const std::vector<std::pair<uint16_t, std::string>> mouse = { { mouseEdge, "" }, { mouseBody, "" } };
        auto mousePicture = [&](std::vector<uint16_t> fills, bool wheelFilled) {
            std::vector<std::pair<uint16_t, std::string>> parts = mouse;
            for (uint16_t f : fills)
                parts.push_back({ f, "" });
            parts.push_back({ mouseLines, "" }), parts.push_back({ wheelEdge, "" });
            if (!wheelFilled)
                parts.push_back({ wheelBody, "" });
            return group(parts);
        };
        mice.push_back(mousePicture({ mouseLeft }, false));
        mice.push_back(mousePicture({}, true));
        mice.push_back(mousePicture({ mouseRight }, false));
        mice.push_back(mousePicture({ wheelUp }, true));
        mice.push_back(mousePicture({ wheelDown }, true));

        // The wrapper: the original art and the variants, and the script that picks one.
        std::vector<Tag> frame;
        frame.push_back(Place(1, group({ { shape, "" } }), "o"));  // (a sprite: scripts cannot reach a shape)
        // The drawn variants sit on the centre of the art's box, where the original button shows (art is not always
        // drawn around its origin: a "Shift + Space" combo came out staggered).
        const Box box = boxOf(shape, 0);
        float cx = box.Empty() ? 0.0f : (box.x0 + box.x1) / 2;
        const float cy = box.Empty() ? 0.0f : (box.y0 + box.y1) / 2;
        if (withCheck)
        {
            // The variant where the A was; a check mark (white, black edge, as the game's) on the right part.
            // (the art's own A is box height tall: the variant, h, is a little smaller and sits a little further left)
            const float artH = box.y1 - box.y0;
            cx = box.x0 + artH / 2 - 3;
            const float kx = box.x0 + artH + (box.x1 - box.x0 - artH) / 2 + 5, ks = artH * 0.36f;
            auto check = [&](float t) {
                // a thick V: from the left arm's top down to the bottom point, then up to the right arm's top
                return Poly{ { { kx - ks - t, cy - t * 0.2f }, { kx - ks * 0.35f, cy + ks * 0.75f + t * 1.2f },
                    { kx + ks * 1.1f + t, cy - ks * 0.85f - t * 0.2f }, { kx + ks * 1.1f - t * 0.9f, cy - ks * 0.85f - t * 1.6f },
                    { kx - ks * 0.35f, cy + ks * 0.75f - t * 2.0f }, { kx - ks + t * 0.4f, cy - t * 1.8f } } };
            };
            // (white only, as the game's: an edge would merge with it under the movies' flat tint)
            const uint16_t body = shapeTag(White, { check(h * 0.09f) });
            frame.push_back(Place(13, group({ { body, "" } }), "k"));
        }
        // Mash ("move any stick": the throw escape): two caps, left and right key, pressed in turn (the script below).
        const bool mash = button == "Mash";
        const float mashOff = mash ? std::max(w, h * 1.15f) * 0.62f : 0.0f;
        frame.push_back(Place(2, cap, "c", cx - mashOff, cy));
        if (mash)
            frame.push_back(Place(21, cap, "c2", cx + mashOff, cy)), frame.push_back(Place(22, text, "t2", cx + mashOff, cy + 1));
        frame.push_back(Place(3, face, "f", cx, cy));
        for (int i = 0; i < 4; i++)
            frame.push_back(Place(uint16_t(4 + i), syms[size_t(i)], "s" + std::to_string(i + 1), cx, cy));
        for (int i = 0; i < 5; i++)
            frame.push_back(Place(uint16_t(8 + i), mice[size_t(i)], "m" + std::to_string(i + 1), cx, cy));
        frame.push_back(Place(20, text, "t", cx - mashOff, cy + 1));
        // Face buttons: each letter's own art (the one closest in size to this container's), for kinds 12-15.
        const bool isFace = std::find(std::begin(Faces), std::end(Faces), button) != std::end(Faces);
        if (isFace)
            for (int i = 0; i < 4; i++)
            {
                const auto& candidates = faceArt[Faces[i]];
                if (candidates.empty())
                    continue;
                uint16_t best = candidates[0];
                float bestDiff = 1e9f;
                for (uint16_t c : candidates)
                {
                    const auto [cw, ch] = sizeOf(c);
                    if (const float d = std::abs(cw - w) + std::abs(ch - h); d < bestDiff)
                        best = c, bestDiff = d;
                }
                // (centred like the drawn variants where the art is not, as in "A ✓"; elsewhere where the art is)
                frame.push_back(Place(uint16_t(14 + i), group({ { best, "" } }), "a" + std::to_string(i + 1),
                    withCheck ? cx : 0.0f, withCheck ? cy : 0.0f));
            }

        // Kinds (_global.dtK_<button>): 0 or none the Wii U art, 1 a cap with text, 2 a face with a letter,
        // 3-6 PlayStation cross, circle, square, triangle, 7-11 mouse left, middle, right, wheel up, wheel down,
        // 12-15 (face buttons) the game's own A, B, X, Y art.
        Script body;
        const int done = body.NewLabel();
        if (mash)
        {
            // Every frame: the left cap is pressed (2 pixels down, dimmer) for 125 ms, then the right one, on the movie's
            // clock (getTimer: the container re-places this wrapper as it animates, so a count of its own restarted).
            auto first = [&body] { body.Op(0x34), body.PushInt(125), body.Op(0x0D), body.Op(0x18), body.PushInt(2), body.Op(0x3F),
                                      body.PushInt(0), body.Op(0x49); };
            auto second = [&body, first] { first(), body.Op(0x12); };
            // (caps move; text fields only dim: a text field's _x/_y place its box's corner, not its centre)
            auto place = [&](const char* child, const std::function<void()>& pressed, float y, bool move) {
                if (move)
                    body.SetChild(child, "_y", [&] { pressed(), body.PushInt(2), body.Op(0x0C), body.PushInt(int(std::lround(y))), body.Op(0x47); });
                body.SetChild(child, "_alpha", [&] { body.PushInt(100), pressed(), body.PushInt(30), body.Op(0x0C), body.Op(0x0B); });
            };
            place("c", first, cy, true), place("t", first, cy + 1, false);
            place("c2", second, cy, true), place("t2", second, cy + 1, false);
        }
        // (applied again on an instance's first frames whatever the generation: a fresh copy's children may not exist yet
        // on the very first call)
        body.This(), body.PushStr("dtN0"), body.This(), body.PushStr("dtN0"), body.GetMember(), body.PushInt(1), body.Op(0x47), body.SetMember();
        body.This(), body.PushStr("dtG"), body.GetMember(), body.PushStr("_global"), body.GetVariable(), body.PushStr("dtPG"),
            body.GetMember(), body.Op(0x49);
        body.This(), body.PushStr("dtN0"), body.GetMember(), body.PushInt(3), body.Op(0x67), body.Op(0x10), body.If(done);
        body.This(), body.PushStr("dtG"), body.PushStr("_global"), body.GetVariable(), body.PushStr("dtPG"), body.GetMember(), body.SetMember();
        body.This(), body.PushStr("dtK"), body.Global("dtK_"), body.SetMember();
        auto is = [&](int kind) { return [&body, kind] { body.Kind(), body.PushInt(kind), body.Op(0x49); }; };
        auto within = [&](int lo, int hi) {
            return [&body, lo, hi] {
                body.Kind(), body.PushInt(lo - 1), body.Op(0x67);   // k > lo - 1
                body.Kind(), body.PushInt(hi + 1), body.Op(0x48);   // k < hi + 1
                body.Op(0x10);                                       // and
            };
        };
        body.SetChild("o", "_visible", [&] { body.Kind(), body.PushInt(0), body.Op(0x67), body.Op(0x12); });  // !(k > 0)
        body.SetChild("c", "_visible", is(1));
        if (withCheck)
            body.SetChild("k", "_visible", [&] { body.Kind(), body.PushInt(0), body.Op(0x67); });  // k > 0
        body.SetChild("f", "_visible", is(2));
        for (int i = 0; i < 4; i++)
            body.SetChild("s" + std::to_string(i + 1), "_visible", is(3 + i));
        for (int i = 0; i < 5; i++)
            body.SetChild("m" + std::to_string(i + 1), "_visible", is(7 + i));
        body.SetChild("t", "_visible", within(1, 2));
        if (isFace)
            for (int i = 0; i < 4; i++)
                if (!faceArt[Faces[i]].empty())
                    body.SetChild("a" + std::to_string(i + 1), "_visible", is(12 + i));
        body.SetChild("t", "htmlText", [&] { body.Global("dtT_"); });
        if (mash)
        {
            body.SetChild("c2", "_visible", is(1)), body.SetChild("t2", "_visible", is(1));
            body.SetChild("t2", "htmlText", [&] { body.Global("dtT2_"); });
            body.SetChild("c2", "_xscale", [&] { body.Global("dtW_"); });
            body.SetChild("t2", "_xscale", [&] { body.Global("dtS_"); });
            body.SetChild("t2", "_yscale", [&] { body.Global("dtS_"); });
        }
        if (outline)
            body.SetChild("t", "textColor", [&] { body.PushInt(0xFFFFFF); });
        body.SetChild("c", "_xscale", [&] { body.Global("dtW_"); });
        body.SetChild("t", "_xscale", [&] { body.Global("dtS_"); });
        body.SetChild("t", "_yscale", [&] { body.Global("dtS_"); });
        // The cap at least as wide as its text, as drawn (the art's size sets the text's: bigger art, bigger letters):
        // c._xscale = max(dtW_, (t.textWidth * dtS_ + pad * 100) / capWidth).
        {
            const int capBase = int(std::max(w, h * 1.15f)), pad = int(h * 0.5f);
            auto fit = [&] {
                body.This(), body.PushStr("t"), body.GetMember(), body.PushStr("textWidth"), body.GetMember();
                body.Global("dtS_"), body.Op(0x0C), body.PushInt(pad * 100), body.Op(0x47), body.PushInt(capBase), body.Op(0x0D);
            };
            const int skip = body.NewLabel();
            fit(), body.This(), body.PushStr("c"), body.GetMember(), body.PushStr("_xscale"), body.GetMember(), body.Op(0x67);
            body.Op(0x12), body.If(skip);
            body.SetChild("c", "_xscale", fit);
            body.Place(skip);
        }
        body.Place(done);
        body.Resolve();

        Script init;
        init.This(), init.PushStr("dtB"), init.PushStr(button), init.SetMember();
        init.This(), init.PushStr("dtG"), init.PushInt(-1), init.SetMember();
        init.This(), init.PushStr("dtN0"), init.PushInt(0), init.SetMember();
        // (registered for the menus' mouse: a click on a prompt presses its button, AddMenuMouse)
        {
            const int have = init.NewLabel();
            init.PushStr("_global"), init.GetVariable(), init.PushStr("dtWrappers"), init.GetMember(), init.If(have);
            init.PushStr("_global"), init.GetVariable(), init.PushStr("dtWrappers"), init.PushInt(0), init.PushStr("Array"), init.Op(0x40),
                init.SetMember();
            init.Place(have);
            init.This(), init.PushInt(1), init.PushStr("_global"), init.GetVariable(), init.PushStr("dtWrappers"), init.GetMember(),
                init.PushStr("push"), init.CallMethod(), init.Pop();
            init.Resolve();
        }
        init.This(), init.PushStr("onEnterFrame"), init.Function(body.code), init.SetMember();
        // Run it at once: a fresh instance would otherwise show every variant (and the Wii U art) for its first frame,
        // and containers re-place their pictures as they animate.
        init.PushInt(0), init.This(), init.PushStr("onEnterFrame"), init.Op(0x52), init.Op(0x17);
        init.Op(0x07);  // stop
        init.Op(0);
        frame.push_back({ TagDoAction, init.code });
        added.push_back(SpriteTag(++next, frame));
        wrapper[{ shape, button }] = next;
    }

    // The rest of each container's pictures (the pressed look, its ring and shine) hide while a variant shows: each
    // unnamed one is placed inside a sprite whose script hides it then. (Named ones are the game's: left alone.)
    std::map<std::pair<uint16_t, std::string>, uint16_t> hider;
    auto Hider = [&](uint16_t character, const std::string& button) {
        auto key = std::make_pair(character, button);
        if (auto it = hider.find(key); it != hider.end())
            return it->second;
        Script body;
        // visible = !(k > 0 && k < 12): the game's own art swapped in (12-15) keeps the container's other pictures
        body.This(), body.PushStr("_visible"), body.Global("dtK_"), body.PushInt(0), body.Op(0x67), body.Global("dtK_"),
            body.PushInt(12), body.Op(0x48), body.Op(0x10), body.Op(0x12), body.SetMember();
        body.Resolve();
        Script init;
        init.This(), init.PushStr("dtB"), init.PushStr(button), init.SetMember();
        init.This(), init.PushStr("onEnterFrame"), init.Function(body.code), init.SetMember();
        init.PushInt(0), init.This(), init.PushStr("onEnterFrame"), init.Op(0x52), init.Op(0x17);  // at once (see above)
        init.Op(0);
        added.push_back(SpriteTag(++next, { Place(1, character, ""), { TagDoAction, init.code } }));
        return hider[key] = next;
    };
    std::map<uint16_t, std::string> containerButton;
    for (auto& [id, name] : exports)
        if (auto it = PromptContainers.find(name); it != PromptContainers.end() && spriteAt.count(id))
            containerButton[id] = it->second.second;

    // Every placement of the art in the library's sprites now places its wrapper.
    for (auto& [id, at] : spriteAt)
    {
        std::vector<Tag> children = inner(id);
        bool changed = false;
        const auto container = containerButton.find(id);
        for (Tag& t : children)
        {
            if (!IsPlacement(t))
                continue;
            Placement p = ReadPlacement(t);
            if (!p.ok || !p.character)
                continue;
            // In a container its own button's wrapper; elsewhere any wrapper of a container's first picture (a pressed
            // stick's parts are other containers' pictures too: they stay as they are inside it).
            // (A copy of the art at another depth, such as Analogstick_L's fading pulse, is hidden with the rest.)
            const bool artSlot = container == containerButton.end() || !artDepthOf.count(id) || artDepthOf[id] == p.depth;
            auto w = container != containerButton.end() && artSlot ? wrapper.find({ *p.character, container->second }) : wrapper.end();
            if (w == wrapper.end() && container == containerButton.end() && firstArt.count(*p.character))
                w = std::find_if(wrapper.begin(), wrapper.end(), [&](const auto& e) { return e.first.first == *p.character; });
            if (w != wrapper.end())
                p.character = w->second, t = WritePlacement(p), changed = true;
            // (named copies of the art too: Analogstick_ANY_move names its tilted sticks; the hider keeps the name)
            else if (container != containerButton.end() && !keepOthers.count(id) &&
                (p.name.empty() || art.count({ *p.character, container->second })) &&
                (shapeSize.count(*p.character) || spriteAt.count(*p.character)))
                p.character = Hider(*p.character, container->second), t = WritePlacement(p), changed = true;
        }
        if (!changed)
            continue;
        Tag& s = tags[at];
        Bytes b(s.body.begin(), s.body.begin() + 4);
        WriteTags(b, children);
        s.body = std::move(b);
    }
    // New definitions before the first sprite that uses them (all art shapes come before their containers).
    size_t first = tags.size();
    for (auto& [id, at] : spriteAt)
        first = std::min(first, at);
    tags.insert(tags.begin() + long(first), added.begin(), added.end());

    Bytes out(movie.begin(), movie.begin() + long(start));
    WriteTags(out, tags);
    const uint32_t size = uint32_t(out.size());
    out[4] = uint8_t(size), out[5] = uint8_t(size >> 8), out[6] = uint8_t(size >> 16), out[7] = uint8_t(size >> 24);
    movie = std::move(out);
    return true;
}
bool UseLibraryPrompts(std::vector<uint8_t>& movie, const std::vector<LocalPrompt>& prompts)
{
    if (movie.size() < 32 || movie[1] != 'F' || (movie[0] != 'G' && movie[0] != 'F'))
        return false;
    const size_t rectBits = 5 + 4 * size_t(movie[8] >> 3);
    const size_t start = 8 + (rectBits + 7) / 8 + 4;
    std::vector<Tag> tags = ParseTags(movie.data() + start, movie.size() - start);
    std::map<uint16_t, size_t> spriteAt;
    std::map<std::string, uint16_t> exported;
    std::set<uint16_t> shapes;  // small ones: button pictures (40 pixels or less)
    uint16_t maxId = 0;
    for (size_t i = 0; i < tags.size(); i++)
    {
        const Tag& t = tags[i];
        if (DefineTags.count(t.code) && t.body.size() >= 2)
        {
            const uint16_t id = U16(t.body.data());
            maxId = std::max(maxId, id);
            if (t.code == TagDefineSprite)
                spriteAt[id] = i;
            if (t.code == 2 || t.code == 22 || t.code == 32 || t.code == 83)
            {
                BitReader r{ &t.body[2] };
                const int n = int(r.U(5));
                const float x0 = float(r.S(n)) / 20, x1 = float(r.S(n)) / 20, y0 = float(r.S(n)) / 20, y1 = float(r.S(n)) / 20;
                if (x1 - x0 <= 40 && y1 - y0 <= 40)
                    shapes.insert(id);
            }
        }
        if (t.code == TagExportAssets && t.body.size() >= 2)
            for (size_t o = 2, k = 0; k < U16(t.body.data()) && o + 2 < t.body.size(); k++)
            {
                const auto end = std::find(t.body.begin() + long(o + 2), t.body.end(), 0);
                exported[std::string(t.body.begin() + long(o + 2), end)] = U16(&t.body[o]);
                o = size_t(end - t.body.begin()) + 1;
            }
        if (t.code == TagImportAssets2)
        {
            size_t o = size_t(std::find(t.body.begin(), t.body.end(), 0) - t.body.begin()) + 1 + 2;
            for (uint16_t k = 0, n = o + 2 <= t.body.size() ? U16(&t.body[o]) : 0; k < n && o + 4 < t.body.size(); k++)
            {
                o += k ? 0 : 2;
                maxId = std::max(maxId, U16(&t.body[o]));
                o = size_t(std::find(t.body.begin() + long(o + 2), t.body.end(), 0) - t.body.begin()) + 1;
            }
        }
    }
    auto inner = [&](uint16_t sprite) { return ParseTags(tags[spriteAt[sprite]].body.data() + 4, tags[spriteAt[sprite]].body.size() - 4); };
    auto setInner = [&](uint16_t sprite, const std::vector<Tag>& children) {
        Tag& s = tags[spriteAt[sprite]];
        Bytes b(s.body.begin(), s.body.begin() + 4);
        WriteTags(b, children);
        s.body = std::move(b);
    };
    auto holderOf = [&](const std::string& name) -> std::optional<uint16_t> {
        if (auto it = exported.find(name); it != exported.end() && spriteAt.count(it->second))
            return it->second;
        for (auto& [id, at] : spriteAt)
            for (const Tag& t : inner(id))
                if (IsPlacement(t))
                    if (const Placement p = ReadPlacement(t); p.ok && p.name == name && p.character && spriteAt.count(*p.character))
                        return *p.character;
        return std::nullopt;
    };
    // The art: a sprite inside the holder whose only placement is a shape (the 32x32 button picture).
    std::vector<std::pair<uint16_t, std::string>> targets;  // art sprite, container
    for (const LocalPrompt& lp : prompts)
    {
        const auto holder = holderOf(lp.holder);
        if (!holder)
            return false;
        std::optional<uint16_t> art;
        for (const Tag& t : inner(*holder))
            if (IsPlacement(t))
                if (const Placement p = ReadPlacement(t); p.ok && p.character && spriteAt.count(*p.character))
                {
                    int places = 0;
                    bool shape = false;
                    for (const Tag& c : inner(*p.character))
                        if (IsPlacement(c))
                            if (const Placement q = ReadPlacement(c); q.ok && q.character)
                                places++, shape = shapes.count(*q.character) > 0;
                    if (places == 1 && shape && !art)
                        art = *p.character;
                }
        if (!art)
            return false;
        targets.push_back({ *art, lp.container });
    }
    // Import the containers and place each, scaled from the library's 40 pixels to the 32 of the art it replaces.
    Bytes import;
    PutStr(import, "../UI_Common/ButtonLibrary.swf");
    import.push_back(1), import.push_back(0);
    Put16(import, uint32_t(targets.size()));
    for (auto& [art, container] : targets)
    {
        const uint16_t id = ++maxId;
        Put16(import, id), PutStr(import, container);
        std::vector<Tag> children = inner(art);
        for (Tag& c : children)
            if (IsPlacement(c))
            {
                Placement p = ReadPlacement(c);
                if (!p.ok)
                    return false;
                Matrix m = p.matrix.value_or(Matrix{});
                m.scale = true, m.sx = m.sy = int32_t(0.8 * 65536);
                p.character = id, p.matrix = m;
                c = WritePlacement(p);
            }
        setInner(art, children);
    }
    size_t first = tags.size();
    for (auto& [id, at] : spriteAt)
        first = std::min(first, at);
    tags.insert(tags.begin() + long(first), Tag{ TagImportAssets2, import });

    Bytes out(movie.begin(), movie.begin() + long(start));
    WriteTags(out, tags);
    const uint32_t size = uint32_t(out.size());
    out[4] = uint8_t(size), out[5] = uint8_t(size >> 8), out[6] = uint8_t(size >> 16), out[7] = uint8_t(size >> 24);
    movie = std::move(out);
    return true;
}
} // namespace assets::swf_patch

namespace assets::swf_patch
{
// A text field's box grown by `left`/`right` pixels and its font scaled by `fontScale`: the DefineEditText that the
// exported sprite `spriteExport` places as `child`. The title's "START" plate holds "Press any key" this way.
bool ResizeEditText(std::vector<uint8_t>& movie, const std::string& spriteExport, const std::string& child, float left,
    float right, float fontScale, float down)
{
    if (movie.size() < 32 || movie[1] != 'F' || (movie[0] != 'G' && movie[0] != 'F'))
        return false;
    const size_t rectBits = 5 + 4 * size_t(movie[8] >> 3);
    const size_t start = 8 + (rectBits + 7) / 8 + 4;
    std::vector<Tag> tags = ParseTags(movie.data() + start, movie.size() - start);
    std::map<uint16_t, size_t> spriteAt, editAt;
    std::map<std::string, uint16_t> exported;
    for (size_t i = 0; i < tags.size(); i++)
    {
        const Tag& t = tags[i];
        if (t.code == TagDefineSprite && t.body.size() >= 4)
            spriteAt[U16(t.body.data())] = i;
        if (t.code == TagDefineEditText && t.body.size() >= 4)
            editAt[U16(t.body.data())] = i;
        if (t.code == TagExportAssets && t.body.size() >= 2)
            for (size_t o = 2, k = 0; k < U16(t.body.data()) && o + 2 < t.body.size(); k++)
            {
                const auto end = std::find(t.body.begin() + long(o + 2), t.body.end(), 0);
                exported[std::string(t.body.begin() + long(o + 2), end)] = U16(&t.body[o]);
                o = size_t(end - t.body.begin()) + 1;
            }
    }
    const auto sprite = exported.find(spriteExport);
    if (sprite == exported.end() || !spriteAt.count(sprite->second))
        return false;
    const Tag& s = tags[spriteAt[sprite->second]];
    std::optional<uint16_t> field;
    for (const Tag& t : ParseTags(s.body.data() + 4, s.body.size() - 4))
        if (IsPlacement(t))
            if (const Placement p = ReadPlacement(t); p.ok && p.name == child && p.character && editAt.count(*p.character))
                field = *p.character;
    if (!field)
        return false;
    Bytes& b = tags[editAt[*field]].body;
    BitReader r{ &b[2] };
    const int n = int(r.U(5));
    int32_t x0 = r.S(n), x1 = r.S(n);
    int32_t y0 = r.S(n), y1 = r.S(n);
    const size_t restAt = 2 + r.Bytes();
    dtlog::Important("[gfx] %s.%s: text box %.0f..%.0f x %.0f..%.0f px\n", spriteExport.c_str(), child.c_str(), x0 / 20.0f,
        x1 / 20.0f, y0 / 20.0f, y1 / 20.0f);
    x0 -= int32_t(left * 20), x1 += int32_t(right * 20);
    y0 += int32_t(down * 20), y1 += int32_t(down * 20);
    const int m = std::max({ BitsFor(x0), BitsFor(x1), BitsFor(y0), BitsFor(y1) });
    BitWriter w;
    w.U(uint32_t(m), 5);
    for (int32_t v : { x0, x1, y0, y1 })
        w.U(uint32_t(v), m);
    Bytes out(b.begin(), b.begin() + 2);
    out.insert(out.end(), w.out.begin(), w.out.end());
    const size_t flagsAt = out.size();
    out.insert(out.end(), b.begin() + long(restAt), b.end());
    // The font height: after the flags, the font id (HasFont) or font class (HasFontClass).
    const uint8_t f1 = out[flagsAt], f2 = out[flagsAt + 1];
    size_t o = flagsAt + 2;
    if (f1 & 0x01)
        o += 2;
    if (f2 & 0x80)
        o = size_t(std::find(out.begin() + long(o), out.end(), 0) - out.begin()) + 1;
    if (((f1 & 0x01) || (f2 & 0x80)) && o + 2 <= out.size())
        Set16(out, o, uint32_t(std::lround(float(U16(&out[o])) * fontScale)));
    b = std::move(out);
    Bytes rebuilt(movie.begin(), movie.begin() + long(start));
    WriteTags(rebuilt, tags);
    const uint32_t size = uint32_t(rebuilt.size());
    rebuilt[4] = uint8_t(size), rebuilt[5] = uint8_t(size >> 8), rebuilt[6] = uint8_t(size >> 16), rebuilt[7] = uint8_t(size >> 24);
    movie = std::move(rebuilt);
    return true;
}

bool RemoveDepth(std::vector<uint8_t>& movie, const std::string& spriteExport, uint16_t depth)
{
    if (movie.size() < 32 || movie[1] != 'F' || (movie[0] != 'G' && movie[0] != 'F'))
        return false;
    const size_t rectBits = 5 + 4 * size_t(movie[8] >> 3);
    const size_t start = 8 + (rectBits + 7) / 8 + 4;
    std::vector<Tag> tags = ParseTags(movie.data() + start, movie.size() - start);
    std::map<uint16_t, size_t> spriteAt;
    std::optional<uint16_t> sprite;
    for (size_t i = 0; i < tags.size(); i++)
    {
        const Tag& t = tags[i];
        if (t.code == TagDefineSprite && t.body.size() >= 4)
            spriteAt[U16(t.body.data())] = i;
        if (t.code == TagExportAssets && t.body.size() >= 2)
            for (size_t o = 2, k = 0; k < U16(t.body.data()) && o + 2 < t.body.size(); k++)
            {
                const auto end = std::find(t.body.begin() + long(o + 2), t.body.end(), 0);
                if (std::string(t.body.begin() + long(o + 2), end) == spriteExport)
                    sprite = U16(&t.body[o]);
                o = size_t(end - t.body.begin()) + 1;
            }
    }
    if (!sprite || !spriteAt.count(*sprite))
        return false;
    Tag& s = tags[spriteAt[*sprite]];
    std::vector<Tag> children = ParseTags(s.body.data() + 4, s.body.size() - 4), kept;
    for (const Tag& t : children)
        if (!IsPlacement(t) || ReadPlacement(t).depth != depth)
            kept.push_back(t);
    if (kept.size() == children.size())
        return false;
    Bytes b(s.body.begin(), s.body.begin() + 4);
    WriteTags(b, kept);
    s.body = std::move(b);
    Bytes rebuilt(movie.begin(), movie.begin() + long(start));
    WriteTags(rebuilt, tags);
    const uint32_t size = uint32_t(rebuilt.size());
    rebuilt[4] = uint8_t(size), rebuilt[5] = uint8_t(size >> 8), rebuilt[6] = uint8_t(size >> 16), rebuilt[7] = uint8_t(size >> 24);
    movie = std::move(rebuilt);
    return true;
}

// The Score Attack loadout's slots (the Soloplay menu's exported "slot" sprite: slot_primary, slot_secondary, slot_melee)
// turn light blue, as the HUD shows X-gear weapons, while _global.dtXGear has their bit (1 primary, 2 secondary, 4 melee; set by
// gfx_hooks.cpp from the menu's selection). Each slot gets an empty child clip whose onEnterFrame sets a colour transform
// on the slot when the bit changes, so the slot's own scripts are untouched.
bool AddXGearTint(std::vector<uint8_t>& movie)
{
    if (movie.size() < 32 || movie[1] != 'F' || (movie[0] != 'G' && movie[0] != 'F'))
        return false;
    const size_t rectBits = 5 + 4 * size_t(movie[8] >> 3);
    const size_t start = 8 + (rectBits + 7) / 8 + 4;
    std::vector<Tag> tags = ParseTags(movie.data() + start, movie.size() - start);
    std::map<uint16_t, size_t> spriteAt;
    std::optional<uint16_t> sprite;
    for (size_t i = 0; i < tags.size(); i++)
    {
        const Tag& t = tags[i];
        if (t.code == TagDefineSprite && t.body.size() >= 4)
            spriteAt[U16(t.body.data())] = i;
        if (t.code == TagExportAssets && t.body.size() >= 2)
            for (size_t o = 2, k = 0; k < U16(t.body.data()) && o + 2 < t.body.size(); k++)
            {
                const auto end = std::find(t.body.begin() + long(o + 2), t.body.end(), 0);
                if (std::string(t.body.begin() + long(o + 2), end) == "slot")
                    sprite = U16(&t.body[o]);
                o = size_t(end - t.body.begin()) + 1;
            }
    }
    if (!sprite || !spriteAt.count(*sprite))
        return false;
    // A colour transform object: { ra, rb, ga, gb, ba, bb, aa, ab } (percent, offset).
    auto transform = [](Script& a, int ra, int rb, int ga, int gb, int ba, int bb) {
        const std::pair<const char*, int> fields[] = { { "ra", ra }, { "rb", rb }, { "ga", ga }, { "gb", gb }, { "ba", ba },
            { "bb", bb }, { "aa", 100 }, { "ab", 0 } };
        for (const auto& [name, value] : fields)
            a.PushStr(name), a.PushInt(value);
        a.PushInt(8), a.Op(0x43);  // ActionInitObject
    };
    // onEnterFrame (this: the child clip): v = _global.dtXGear & this.dtBit; when it differs from the slot's dtXG, store it
    // and set the slot's colour transform.
    Script body;
    const int done = body.NewLabel(), plain = body.NewLabel(), set = body.NewLabel();
    body.This(), body.PushStr("dtV");
    body.PushStr("_global"), body.GetVariable(), body.PushStr("dtXGear"), body.GetMember();
    body.This(), body.PushStr("dtBit"), body.GetMember(), body.Op(0x60);  // BitAnd
    body.SetMember();
    body.This(), body.PushStr("dtV"), body.GetMember();
    body.This(), body.PushStr("_parent"), body.GetMember(), body.PushStr("dtXG"), body.GetMember();
    body.Op(0x49), body.If(done);  // Equals2
    body.This(), body.PushStr("_parent"), body.GetMember(), body.PushStr("dtXG"), body.This(), body.PushStr("dtV"),
        body.GetMember(), body.SetMember();
    body.This(), body.PushStr("dtV"), body.GetMember(), body.Op(0x12), body.If(plain);
    transform(body, 30, 0, 85, 95, 60, 190);  // light blue (the HUD's X-gear colour)
    body.Jump(set);
    body.Place(plain);
    transform(body, 100, 0, 100, 0, 100, 0);
    body.Place(set);
    // new Color(this._parent).setTransform(t)
    body.PushInt(1);
    body.This(), body.PushStr("_parent"), body.GetMember(), body.PushInt(1), body.PushStr("Color"), body.Op(0x40);  // NewObject
    body.PushStr("setTransform"), body.CallMethod(), body.Pop();
    body.Place(done);
    body.Resolve();
    // Frame 1 of each slot, once: the clip, its bit (by the slot's instance name) and its onEnterFrame.
    Script init;
    const int skip = init.NewLabel(), notPrimary = init.NewLabel(), named = init.NewLabel();
    init.This(), init.PushStr("dtXGC"), init.GetMember(), init.Op(0x12), init.Op(0x12), init.If(skip);
    init.This(), init.PushStr("dtXG"), init.PushInt(0), init.SetMember();
    init.This(), init.PushStr("dtXGC");
    init.PushInt(16000), init.PushStr("dtXGearClip"), init.PushInt(2), init.This(), init.PushStr("createEmptyMovieClip"),
        init.CallMethod();
    init.SetMember();
    init.This(), init.PushStr("dtXGC"), init.GetMember(), init.PushStr("dtBit"), init.PushInt(0), init.SetMember();
    init.This(), init.PushStr("_name"), init.GetMember(), init.PushStr("slot_primary"), init.Op(0x49), init.Op(0x12),
        init.If(notPrimary);
    init.This(), init.PushStr("dtXGC"), init.GetMember(), init.PushStr("dtBit"), init.PushInt(1), init.SetMember();
    init.Jump(named);
    init.Place(notPrimary);
    const int notSecondary = init.NewLabel();
    init.This(), init.PushStr("_name"), init.GetMember(), init.PushStr("slot_secondary"), init.Op(0x49), init.Op(0x12),
        init.If(notSecondary);
    init.This(), init.PushStr("dtXGC"), init.GetMember(), init.PushStr("dtBit"), init.PushInt(2), init.SetMember();
    init.Jump(named);
    init.Place(notSecondary);
    init.This(), init.PushStr("_name"), init.GetMember(), init.PushStr("slot_melee"), init.Op(0x49), init.Op(0x12),
        init.If(named);
    init.This(), init.PushStr("dtXGC"), init.GetMember(), init.PushStr("dtBit"), init.PushInt(4), init.SetMember();
    init.Place(named);
    init.This(), init.PushStr("dtXGC"), init.GetMember(), init.PushStr("onEnterFrame"), init.Function(body.code), init.SetMember();
    init.Place(skip);
    init.Op(0);
    init.Resolve();
    Tag& s = tags[spriteAt[*sprite]];
    std::vector<Tag> children = ParseTags(s.body.data() + 4, s.body.size() - 4);
    children.insert(children.begin(), Tag{ TagDoAction, init.code });
    Bytes b(s.body.begin(), s.body.begin() + 4);
    WriteTags(b, children);
    s.body = std::move(b);
    Bytes rebuilt(movie.begin(), movie.begin() + long(start));
    WriteTags(rebuilt, tags);
    const uint32_t size = uint32_t(rebuilt.size());
    rebuilt[4] = uint8_t(size), rebuilt[5] = uint8_t(size >> 8), rebuilt[6] = uint8_t(size >> 16), rebuilt[7] = uint8_t(size >> 24);
    movie = std::move(rebuilt);
    return true;
}

// A sprite (by its exported name) drawn with a colour transform: { ra, rb, ga, gb, ba, bb } percent and offset, set by
// a script on its first frame.
bool TintSprite(std::vector<uint8_t>& movie, const std::string& spriteExport, int ra, int rb, int ga, int gb, int ba, int bb)
{
    if (movie.size() < 32 || movie[1] != 'F' || (movie[0] != 'G' && movie[0] != 'F'))
        return false;
    const size_t rectBits = 5 + 4 * size_t(movie[8] >> 3);
    const size_t start = 8 + (rectBits + 7) / 8 + 4;
    std::vector<Tag> tags = ParseTags(movie.data() + start, movie.size() - start);
    std::map<uint16_t, size_t> spriteAt;
    std::optional<uint16_t> sprite;
    for (size_t i = 0; i < tags.size(); i++)
    {
        const Tag& t = tags[i];
        if (t.code == TagDefineSprite && t.body.size() >= 4)
            spriteAt[U16(t.body.data())] = i;
        if (t.code == TagExportAssets && t.body.size() >= 2)
            for (size_t o = 2, k = 0; k < U16(t.body.data()) && o + 2 < t.body.size(); k++)
            {
                const auto end = std::find(t.body.begin() + long(o + 2), t.body.end(), 0);
                if (std::string(t.body.begin() + long(o + 2), end) == spriteExport)
                    sprite = U16(&t.body[o]);
                o = size_t(end - t.body.begin()) + 1;
            }
    }
    if (!sprite || !spriteAt.count(*sprite))
        return false;
    // new Color(this).setTransform({ ra, rb, ga, gb, ba, bb, aa: 100, ab: 0 })
    Script a;
    const std::pair<const char*, int> fields[] = { { "ra", ra }, { "rb", rb }, { "ga", ga }, { "gb", gb }, { "ba", ba }, { "bb", bb },
        { "aa", 100 }, { "ab", 0 } };
    for (const auto& [name, value] : fields)
        a.PushStr(name), a.PushInt(value);
    a.PushInt(8), a.Op(0x43);
    a.PushInt(1), a.This(), a.PushInt(1), a.PushStr("Color"), a.Op(0x40), a.PushStr("setTransform"), a.CallMethod(), a.Pop();
    a.Op(0);
    Tag& s = tags[spriteAt[*sprite]];
    std::vector<Tag> children = ParseTags(s.body.data() + 4, s.body.size() - 4);
    children.insert(children.begin(), Tag{ TagDoAction, a.code });
    Bytes b(s.body.begin(), s.body.begin() + 4);
    WriteTags(b, children);
    s.body = std::move(b);
    Bytes rebuilt(movie.begin(), movie.begin() + long(start));
    WriteTags(rebuilt, tags);
    const uint32_t size = uint32_t(rebuilt.size());
    rebuilt[4] = uint8_t(size), rebuilt[5] = uint8_t(size >> 8), rebuilt[6] = uint8_t(size >> 16), rebuilt[7] = uint8_t(size >> 24);
    movie = std::move(rebuilt);
    return true;
}

bool AddMenuMouse(std::vector<uint8_t>& movie)
{
    if (movie.size() < 32 || movie[1] != 'F' || (movie[0] != 'G' && movie[0] != 'F'))
        return false;
    const size_t rectBits = 5 + 4 * size_t(movie[8] >> 3);
    const size_t start = 8 + (rectBits + 7) / 8 + 4;
    std::vector<Tag> tags = ParseTags(movie.data() + start, movie.size() - start);
    const auto firstFrame = std::find_if(tags.begin(), tags.end(), [](const Tag& t) { return t.code == 1; });
    if (firstFrame == tags.end())
        return false;
    auto G = [](Script& a, const char* name) { a.PushStr("_global"), a.GetVariable(), a.PushStr(name), a.GetMember(); };
    auto SetG = [](Script& a, const char* name, const std::function<void()>& value) {
        a.PushStr("_global"), a.GetVariable(), a.PushStr(name), value(), a.SetMember();
    };
    auto Undefined = [](Script& a) { a.Action(0x96, Bytes{ 3 }); };

    // onMouseMove: the CLIK button under the pointer (Mouse.getTopMostEntity(false), up through its parents to the first
    // clip with handleMousePress) takes the focus, as the game's D3Button.OnRollOver does: Selection.setFocus(button).
    // The object under the pointer, in _global.dtT: Mouse.getTopMostEntity(all) (false: objects with button handlers
    // only; the menus lay a transparent full-screen clip over everything). Menus keep hidden sub-menus at alpha 0 over the
    // visible ones: a hit inside anything invisible (an ancestor at _alpha 0 or not _visible), and with `all` false one
    // outside any button, is hidden for a moment and the query repeats (at most 12 times); all are shown again after.
    // `member`: what makes a hit count, on it or an ancestor (a CLIK button's handleMousePress, a prompt wrapper's dtB);
    // anything else under the pointer is looked past. `all`: Mouse.getTopMostEntity's argument.
    auto pick = [&](Script& move, bool all, const char* member) {
        const int query = move.NewLabel(), check = move.NewLabel(), clear = move.NewLabel(), checked = move.NewLabel(),
                  restore = move.NewLabel(), restored = move.NewLabel();
        SetG(move, "dtHid", [&] { move.PushInt(0), move.PushStr("Array"), move.Op(0x40); });
        SetG(move, "dtN", [&] { move.PushInt(0); });
        move.Place(query);
        SetG(move, "dtT", [&] {
            move.Action(0x96, Bytes{ 5, uint8_t(all) }), move.PushInt(1), move.PushStr("Mouse"), move.GetVariable(),
                move.PushStr("getTopMostEntity"), move.CallMethod();
        });
        G(move, "dtT"), move.Op(0x12), move.If(restore);
        SetG(move, "dtA", [&] { G(move, "dtT"); });
        SetG(move, "dtB", [&] { move.Action(0x96, Bytes{ 5, 0 }); });  // (inside something with `member`)
        move.Place(check);
        G(move, "dtA"), move.Op(0x12), move.If(checked);
        {
            const int notButton = move.NewLabel();
            G(move, "dtA"), move.PushStr(member), move.GetMember(), move.Op(0x12), move.If(notButton);
            SetG(move, "dtB", [&] { move.Action(0x96, Bytes{ 5, 1 }); });
            move.Place(notButton);
        }
        G(move, "dtA"), move.PushStr("_visible"), move.GetMember(), move.Op(0x12), move.If(clear);
        G(move, "dtA"), move.PushStr("_alpha"), move.GetMember(), move.PushInt(0), move.Op(0x49), move.If(clear);
        SetG(move, "dtA", [&] { G(move, "dtA"), move.PushStr("_parent"), move.GetMember(); });
        move.Jump(check);
        move.Place(clear);
        G(move, "dtN"), move.PushInt(40), move.Op(0x48), move.Op(0x12), move.If(restore);
        G(move, "dtT"), move.PushStr("_visible"), move.Action(0x96, Bytes{ 5, 0 }), move.SetMember();
        G(move, "dtT"), move.PushInt(1), G(move, "dtHid"), move.PushStr("push"), move.CallMethod(), move.Pop();
        SetG(move, "dtN", [&] { G(move, "dtN"), move.PushInt(1), move.Op(0x47); });
        SetG(move, "dtT", [&] { Undefined(move); });
        move.Jump(query);
        move.Place(checked);
        // (visible but not part of a button, such as another layer's text over this one: look below it too)
        G(move, "dtB"), move.Op(0x12), move.If(clear);
        move.Place(restore);
        G(move, "dtHid"), move.PushStr("length"), move.GetMember(), move.PushInt(0), move.Op(0x67), move.Op(0x12), move.If(restored);
        move.PushInt(0), G(move, "dtHid"), move.PushStr("pop"), move.CallMethod(), move.PushStr("_visible"), move.Action(0x96, Bytes{ 5, 1 }),
            move.SetMember();
        move.Jump(restore);
        move.Place(restored);
    };

    // onMouseMove: the CLIK button under the pointer (up through its parents to the first clip with handleMousePress)
    // takes the focus, as the game's D3Button.OnRollOver does: Selection.setFocus(button).
    Script move;
    {
        const int loop = move.NewLabel(), found = move.NewLabel(), none = move.NewLabel(), end = move.NewLabel();
        pick(move, false, "handleMousePress");
        move.Place(loop);
        G(move, "dtT"), move.Op(0x12), move.If(none);
        G(move, "dtT"), move.PushStr("handleMousePress"), move.GetMember(), move.Op(0x12), move.Op(0x12), move.If(found);
        SetG(move, "dtT", [&] { G(move, "dtT"), move.PushStr("_parent"), move.GetMember(); });
        move.Jump(loop);
        move.Place(found);
        G(move, "dtT"), G(move, "dtHover"), move.Op(0x49), move.If(end);
        SetG(move, "dtHover", [&] { G(move, "dtT"); });
        SetG(move, "dtLast", [&] { G(move, "dtT"); });  // (the row a click beside it belongs to: see the arrows below)
        G(move, "dtT"), move.PushStr("_disabled"), move.GetMember(), move.If(end);
        G(move, "dtT"), move.PushInt(1), move.PushStr("Selection"), move.GetVariable(), move.PushStr("setFocus"), move.CallMethod(), move.Pop();
        move.Jump(end);
        move.Place(none);
        SetG(move, "dtHover", [&] { Undefined(move); });
        move.Place(end);
        move.Resolve();
    }
    // A click: the host never passes mouse buttons to the movies (where nothing covers a button the game's own click
    // worked, so ours confirmed twice; in other menus only ours does anything); it counts clicks in _global.dtClick
    // instead (PointerTick), and an empty clip's frame script confirms the button under the pointer once per new count
    // (ExternalInterface DtPortClick presses the GamePad's A, the menus' confirm).
    Script poll;
    {
        const int end = poll.NewLabel();
        G(poll, "dtClick"), G(poll, "dtClickSeen"), poll.Op(0x49), poll.If(end);
        SetG(poll, "dtClickSeen", [&] { G(poll, "dtClick"); });
        // (arrows first: the slot's own hit area covers its arrows, so a click on one also finds the slot hovered)
        const int beside = poll.NewLabel(), click = poll.NewLabel();
        poll.Jump(beside);
        poll.Place(click);
        {
            // (not on a button: a visible button prompt under the pointer presses its button; prompts are found by their
            // bounds, as the hit test does not see the prompt art in every movie)
            const int onButton = poll.NewLabel();
            G(poll, "dtHover"), poll.Op(0x12), poll.Op(0x12), poll.If(onButton);
            SetG(poll, "dtPrompt", [&] { Undefined(poll); });
            {
                const int loop = poll.NewLabel(), next = poll.NewLabel(), done = poll.NewLabel(), vis = poll.NewLabel(),
                          visOk = poll.NewLabel();
                G(poll, "dtWrappers"), poll.Op(0x12), poll.If(done);
                SetG(poll, "dtI", [&] { G(poll, "dtWrappers"), poll.PushStr("length"), poll.GetMember(); });
                poll.Place(loop);
                G(poll, "dtI"), poll.PushInt(1), poll.Op(0x48), poll.If(done);  // i < 1: done
                SetG(poll, "dtI", [&] { G(poll, "dtI"), poll.PushInt(1), poll.Op(0x0B); });
                SetG(poll, "dtW", [&] { G(poll, "dtWrappers"), G(poll, "dtI"), poll.GetMember(); });
                // (a removed clip has no _name)
                G(poll, "dtW"), poll.PushStr("_name"), poll.GetMember(), poll.Op(0x12), poll.If(next);
                // visible: the wrapper and every ancestor _visible with _alpha > 0
                SetG(poll, "dtA", [&] { G(poll, "dtW"); });
                poll.Place(vis);
                G(poll, "dtA"), poll.Op(0x12), poll.If(visOk);
                G(poll, "dtA"), poll.PushStr("_visible"), poll.GetMember(), poll.Op(0x12), poll.If(next);
                G(poll, "dtA"), poll.PushStr("_alpha"), poll.GetMember(), poll.PushInt(1), poll.Op(0x48), poll.If(next);
                SetG(poll, "dtA", [&] { G(poll, "dtA"), poll.PushStr("_parent"), poll.GetMember(); });
                poll.Jump(vis);
                poll.Place(visOk);
                SetG(poll, "dtBox", [&] {
                    poll.PushStr("_root"), poll.GetVariable(), poll.PushInt(1), G(poll, "dtW"), poll.PushStr("getBounds"), poll.CallMethod();
                });
                auto box = [&](const char* edge) { G(poll, "dtBox"), poll.PushStr(edge), poll.GetMember(); };
                auto mouse = [&](const char* axis) { poll.PushStr("_root"), poll.GetVariable(), poll.PushStr(axis), poll.GetMember(); };
                mouse("_xmouse"), box("xMin"), poll.PushInt(6), poll.Op(0x0B), poll.Op(0x48), poll.If(next);
                box("xMax"), poll.PushInt(6), poll.Op(0x47), mouse("_xmouse"), poll.Op(0x48), poll.If(next);
                mouse("_ymouse"), box("yMin"), poll.PushInt(6), poll.Op(0x0B), poll.Op(0x48), poll.If(next);
                box("yMax"), poll.PushInt(6), poll.Op(0x47), mouse("_ymouse"), poll.Op(0x48), poll.If(next);
                SetG(poll, "dtPrompt", [&] { G(poll, "dtW"), poll.PushStr("dtB"), poll.GetMember(); });
                poll.Jump(done);
                poll.Place(next);
                poll.Jump(loop);
                poll.Place(done);
            }
            for (const char* b : { "A", "B", "X", "Y", "Plus", "Minus" })
            {
                const int no = poll.NewLabel();
                G(poll, "dtPrompt"), poll.PushStr(b), poll.Op(0x49), poll.Op(0x12), poll.If(no);
                poll.CallExternal(std::string(b) == "A" ? "DtPortClick" : "DtPortPress" + std::string(b));
                poll.Jump(end);
                poll.Place(no);
            }
            poll.Jump(end);
            poll.Place(onButton);
        }
        poll.CallExternal("DtPortClick");
        poll.Jump(end);
        // On or near (24 pixels) one of the arrows inside the last button hovered (a child holding
        // arrow_left_mc / arrow_right_mc: the weapon loadout's slots, whose "<" ">" are thin shapes, not buttons) presses
        // the D-pad that way (DtPortLeft / DtPortRight).
        poll.Place(beside);
        {
            G(poll, "dtLast"), poll.Op(0x12), poll.If(click);
            SetG(poll, "dtArrows", [&] { poll.Action(0x96, Bytes{ 3 }); });
            const int each = poll.NewLabel(), last = poll.NewLabel(), skip = poll.NewLabel();
            G(poll, "dtLast"), poll.Op(0x55);  // Enumerate2: a null, then every member name
            poll.Place(each);
            poll.Op(0x4C), poll.Action(0x96, Bytes{ 2 }), poll.Op(0x49), poll.If(last);  // (null: the end)
            // dtC = dtLast[name] (a timeline variable: the name is on the stack under it)
            G(poll, "dtLast"), poll.Op(0x4D), poll.GetMember(), poll.PushStr("dtC"), poll.Op(0x4D), poll.Op(0x1D);
            poll.PushStr("dtC"), poll.GetVariable(), poll.PushStr("arrow_left_mc"), poll.GetMember(), poll.Op(0x12), poll.If(skip);
            SetG(poll, "dtArrows", [&] { poll.PushStr("dtC"), poll.GetVariable(); });
            poll.Place(skip);
            poll.Jump(each);
            poll.Place(last);
            poll.Pop();
            G(poll, "dtArrows"), poll.Op(0x12), poll.If(click);
            auto mouse = [&](const char* axis) { poll.PushStr("_root"), poll.GetVariable(), poll.PushStr(axis), poll.GetMember(); };
            auto near = [&](const char* arrow, const char* external) {  // pointer within 24 pixels of the arrow's box
                const int no = poll.NewLabel();
                SetG(poll, "dtBox", [&] {
                    poll.PushStr("_root"), poll.GetVariable(), poll.PushInt(1), G(poll, "dtArrows"), poll.PushStr(arrow), poll.GetMember(),
                        poll.PushStr("getBounds"), poll.CallMethod();
                });
                auto box = [&](const char* edge) { G(poll, "dtBox"), poll.PushStr(edge), poll.GetMember(); };
                mouse("_xmouse"), box("xMin"), poll.PushInt(24), poll.Op(0x0B), poll.Op(0x48), poll.If(no);
                box("xMax"), poll.PushInt(24), poll.Op(0x47), mouse("_xmouse"), poll.Op(0x48), poll.If(no);
                mouse("_ymouse"), box("yMin"), poll.PushInt(24), poll.Op(0x0B), poll.Op(0x48), poll.If(no);
                box("yMax"), poll.PushInt(24), poll.Op(0x47), mouse("_ymouse"), poll.Op(0x48), poll.If(no);
                poll.CallExternal(external);
                poll.Jump(end);
                poll.Place(no);
            };
            near("arrow_left_mc", "DtPortLeft");
            near("arrow_right_mc", "DtPortRight");
            poll.Jump(click);
        }
        poll.Place(end);
        poll.Resolve();
    }
    // Once per movie: _global.dtMouse = {onMouseMove}; Mouse.addListener(_global.dtMouse); and the clicks' clip,
    // _root.createEmptyMovieClip("dtMouseClip", _root.getNextHighestDepth()).onEnterFrame = poll.
    Script init;
    const int skip = init.NewLabel();
    G(init, "dtMouse"), init.Op(0x12), init.Op(0x12), init.If(skip);
    SetG(init, "dtMouse", [&] { init.PushInt(0), init.PushStr("Object"), init.Op(0x40); });
    G(init, "dtMouse"), init.PushStr("onMouseMove"), init.Function(move.code), init.SetMember();
    G(init, "dtMouse"), init.PushInt(1), init.PushStr("Mouse"), init.GetVariable(), init.PushStr("addListener"), init.CallMethod(), init.Pop();
    init.PushInt(0), init.PushStr("_root"), init.GetVariable(), init.PushStr("getNextHighestDepth"), init.CallMethod();
    init.PushStr("dtMouseClip"), init.PushInt(2), init.PushStr("_root"), init.GetVariable(), init.PushStr("createEmptyMovieClip"),
        init.CallMethod(), init.Pop();
    init.PushStr("_root"), init.GetVariable(), init.PushStr("dtMouseClip"), init.GetMember(), init.PushStr("onEnterFrame"),
        init.Function(poll.code), init.SetMember();
    SetG(init, "dtClickSeen", [&] { G(init, "dtClick"); });
    init.Place(skip);
    init.Op(0);
    init.Resolve();
    tags.insert(firstFrame, Tag{ TagDoAction, init.code });

    Bytes out(movie.begin(), movie.begin() + long(start));
    WriteTags(out, tags);
    const uint32_t size = uint32_t(out.size());
    out[4] = uint8_t(size), out[5] = uint8_t(size >> 8), out[6] = uint8_t(size >> 16), out[7] = uint8_t(size >> 24);
    movie = std::move(out);
    return true;
}
} // namespace assets::swf_patch
