#include "ParquetMerge.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <chrono>
#include <functional>

// A Parquet file is "PAR1", the column chunks (plus optional page indexes and bloom filters), the footer
// (FileMetaData, Thrift compact protocol), its length (4 bytes, little endian) and "PAR1". The footer locates every
// chunk by absolute file offset, so joining files is: copy each file's bytes between the magics, then one footer whose
// row groups are those of all files with their offsets shifted by where each file's bytes landed. Parquet's Thrift
// definition is stable; the offsets touched are (field ids of parquet.thrift):
//   FileMetaData   3 num_rows (summed), 4 row_groups
//   RowGroup       1 columns, 5 file_offset, 7 ordinal (renumbered)
//   ColumnChunk    2 file_offset, 3 meta_data, 4 offset_index_offset, 6 column_index_offset
//   ColumnMetaData 9 data_page_offset, 10 index_page_offset, 11 dictionary_page_offset, 14 bloom_filter_offset
// Everything else is parsed generically and written back unchanged.
namespace ODIA
{
  namespace
  {
    enum : std::uint8_t { T_TRUE = 1, T_FALSE = 2, T_BYTE = 3, T_I16 = 4, T_I32 = 5, T_I64 = 6, T_DOUBLE = 7,
                          T_BINARY = 8, T_LIST = 9, T_SET = 10, T_MAP = 11, T_STRUCT = 12 };

    struct Value;
    struct Field { std::int16_t id = 0; std::uint8_t type = 0; std::vector<Value> v; };   // v: one value (none for bools)
    struct Value
    {
      std::int64_t i = 0;              // integers, bool (1 = true) and bool list elements
      std::string bin;                 // binary, double (8 raw bytes)
      std::uint8_t elem = 0, key = 0;  // list/set element type; map key (elem = value) type
      std::vector<Value> items;        // list/set elements; map: key, value, key, value, ...
      std::vector<Field> fields;       // struct
    };

    struct Reader
    {
      const std::uint8_t* p; const std::uint8_t* end;
      std::uint8_t byte() { if (p >= end) { throw std::runtime_error("Parquet footer truncated"); } return *p++; }
      std::uint64_t varint()
      {
        std::uint64_t r = 0;
        for (int s = 0; s < 64; s += 7) { const std::uint8_t b = byte(); r |= std::uint64_t(b & 0x7f) << s; if (!(b & 0x80)) { return r; } }
        throw std::runtime_error("Parquet footer: bad varint");
      }
      std::int64_t zigzag() { const std::uint64_t u = varint(); return static_cast<std::int64_t>(u >> 1) ^ -static_cast<std::int64_t>(u & 1); }

      Value value(std::uint8_t t)
      {
        Value v;
        switch (t)
        {
          case T_TRUE: case T_FALSE: v.i = byte() == T_TRUE; break;   // bool as a list element
          case T_BYTE: v.i = static_cast<std::int8_t>(byte()); break;
          case T_I16: case T_I32: case T_I64: v.i = zigzag(); break;
          case T_DOUBLE: v.bin.assign(reinterpret_cast<const char*>(p), 8); for (int k = 0; k < 8; ++k) { byte(); } break;
          case T_BINARY: { const std::uint64_t n = varint(); if (n > static_cast<std::uint64_t>(end - p)) { throw std::runtime_error("Parquet footer truncated"); }
                           v.bin.assign(reinterpret_cast<const char*>(p), n); p += n; break; }
          case T_LIST: case T_SET:
          {
            const std::uint8_t h = byte();
            std::uint64_t n = h >> 4;
            v.elem = h & 0x0f;
            if (n == 15) { n = varint(); }
            for (std::uint64_t k = 0; k < n; ++k) { v.items.push_back(value(v.elem)); }
            break;
          }
          case T_MAP:
          {
            const std::uint64_t n = varint();
            if (n > 0) { const std::uint8_t kv = byte(); v.key = kv >> 4; v.elem = kv & 0x0f; }
            for (std::uint64_t k = 0; k < n; ++k) { v.items.push_back(value(v.key)); v.items.push_back(value(v.elem)); }
            break;
          }
          case T_STRUCT: v.fields = structFields(); break;
          default: throw std::runtime_error("Parquet footer: unknown Thrift type " + std::to_string(t));
        }
        return v;
      }

      std::vector<Field> structFields()
      {
        std::vector<Field> fs;
        std::int16_t last = 0;
        for (;;)
        {
          const std::uint8_t h = byte();
          if (h == 0) { return fs; }
          Field f;
          f.type = h & 0x0f;
          const std::uint8_t delta = h >> 4;
          f.id = delta ? static_cast<std::int16_t>(last + delta) : static_cast<std::int16_t>(zigzag());
          last = f.id;
          if (f.type == T_TRUE || f.type == T_FALSE) { Value b; b.i = f.type == T_TRUE; f.v.push_back(b); }
          else { f.v.push_back(value(f.type)); }
          fs.push_back(std::move(f));
        }
      }
    };

    struct Writer
    {
      std::string out;
      void byte(std::uint8_t b) { out.push_back(static_cast<char>(b)); }
      void varint(std::uint64_t u) { while (u >= 0x80) { byte(static_cast<std::uint8_t>(u | 0x80)); u >>= 7; } byte(static_cast<std::uint8_t>(u)); }
      void zigzag(std::int64_t n) { varint((static_cast<std::uint64_t>(n) << 1) ^ static_cast<std::uint64_t>(n >> 63)); }

      void value(std::uint8_t t, const Value& v)
      {
        switch (t)
        {
          case T_TRUE: case T_FALSE: byte(v.i ? T_TRUE : T_FALSE); break;
          case T_BYTE: byte(static_cast<std::uint8_t>(v.i)); break;
          case T_I16: case T_I32: case T_I64: zigzag(v.i); break;
          case T_DOUBLE: out += v.bin; break;
          case T_BINARY: varint(v.bin.size()); out += v.bin; break;
          case T_LIST: case T_SET:
            if (v.items.size() < 15) { byte(static_cast<std::uint8_t>(v.items.size() << 4 | v.elem)); }
            else { byte(static_cast<std::uint8_t>(0xf0 | v.elem)); varint(v.items.size()); }
            for (const auto& e : v.items) { value(v.elem, e); }
            break;
          case T_MAP:
            varint(v.items.size() / 2);
            if (!v.items.empty()) { byte(static_cast<std::uint8_t>(v.key << 4 | v.elem)); }
            for (std::size_t k = 0; k < v.items.size(); k += 2) { value(v.key, v.items[k]); value(v.elem, v.items[k + 1]); }
            break;
          case T_STRUCT: structFields(v.fields); break;
          default: throw std::runtime_error("Parquet footer: unknown Thrift type");
        }
      }

      void structFields(const std::vector<Field>& fs)
      {
        std::int16_t last = 0;
        for (const auto& f : fs)
        {
          const bool is_bool = f.type == T_TRUE || f.type == T_FALSE;
          const std::uint8_t t = is_bool ? (f.v[0].i ? T_TRUE : T_FALSE) : f.type;
          if (f.id > last && f.id - last <= 15) { byte(static_cast<std::uint8_t>((f.id - last) << 4 | t)); }
          else { byte(t); zigzag(f.id); }
          last = f.id;
          if (!is_bool) { value(f.type, f.v[0]); }
        }
        byte(0);
      }
    };

    Field* find(std::vector<Field>& fs, std::int16_t id)
    {
      for (auto& f : fs) { if (f.id == id) { return &f; } }
      return nullptr;
    }

    void translate(std::vector<Field>& fs, std::initializer_list<std::int16_t> ids, const std::function<std::int64_t(std::int64_t)>& to_file)
    {
      for (const std::int16_t id : ids)
      {
        if (Field* f = find(fs, id); f && f->type == T_I64 && f->v[0].i > 0) { f->v[0].i = to_file(f->v[0].i); }   // 0: unset
      }
    }
  }

  ParquetJoiner::ParquetJoiner(const std::string& path, std::size_t parts)
    : path_(path), segments_(parts), footers_(parts)
  {
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_ || std::fwrite("PAR1", 1, 4, f_) != 4) { throw std::runtime_error("cannot write " + path); }
  }

  ParquetJoiner::~ParquetJoiner()
  {
    if (f_) { std::fclose(f_); }
  }

  void ParquetJoiner::append(std::size_t part, std::int64_t local, const std::uint8_t* data, std::int64_t n)
  {
    if (n <= 0) { return; }
    const auto t0 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(m_);
    if (std::fwrite(data, 1, static_cast<std::size_t>(n), f_) != static_cast<std::size_t>(n)) { throw std::runtime_error("write failed: " + path_); }
    segments_[part].push_back({local, pos_, n});
    pos_ += n;
    append_s_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  }

  void ParquetJoiner::setFooter(std::size_t part, std::string footer)
  {
    std::lock_guard<std::mutex> lock(m_);
    footers_[part] = std::move(footer);
  }

  void ParquetJoiner::finish()
  {
    std::vector<Field> meta;
    std::int64_t rows = 0;
    Value groups;
    groups.elem = T_STRUCT;
    for (std::size_t k = 0; k < footers_.size(); ++k)
    {
      Reader r{reinterpret_cast<const std::uint8_t*>(footers_[k].data()), reinterpret_cast<const std::uint8_t*>(footers_[k].data()) + footers_[k].size()};
      std::vector<Field> pm = r.structFields();
      const auto& seg = segments_[k];
      const std::function<std::int64_t(std::int64_t)> to_file = [&seg, k](std::int64_t off) {
        for (const Segment& s : seg) { if (off >= s.local && off < s.local + s.n) { return s.file + (off - s.local); } }
        throw std::runtime_error("Parquet join: offset " + std::to_string(off) + " of part " + std::to_string(k) + " not in the file");
      };
      if (Field* n = find(pm, 3)) { rows += n->v[0].i; }
      if (Field* rg = find(pm, 4))
      {
        for (Value& g : rg->v[0].items)
        {
          translate(g.fields, {5}, to_file);
          if (Field* cols = find(g.fields, 1))
          {
            for (Value& c : cols->v[0].items)
            {
              translate(c.fields, {2, 4, 6}, to_file);
              if (Field* md = find(c.fields, 3)) { translate(md->v[0].fields, {9, 10, 11, 14}, to_file); }
            }
          }
          if (Field* ord = find(g.fields, 7)) { ord->v[0].i = static_cast<std::int64_t>(groups.items.size()); }
          groups.items.push_back(std::move(g));
        }
      }
      if (k == 0) { meta = std::move(pm); }
    }
    if (Field* n = find(meta, 3)) { n->v[0].i = rows; }
    Field* rg = find(meta, 4);
    if (!rg) { throw std::runtime_error("Parquet footer without row groups"); }
    rg->v[0] = std::move(groups);
    Writer w;
    w.structFields(meta);
    const std::uint32_t len = static_cast<std::uint32_t>(w.out.size());
    const char tail[8] = {static_cast<char>(len), static_cast<char>(len >> 8), static_cast<char>(len >> 16), static_cast<char>(len >> 24), 'P', 'A', 'R', '1'};
    const bool ok = std::fwrite(w.out.data(), 1, w.out.size(), f_) == w.out.size() && std::fwrite(tail, 1, 8, f_) == 8;
    const bool closed = std::fclose(f_) == 0;
    f_ = nullptr;
    if (!ok || !closed) { throw std::runtime_error("write failed: " + path_); }
  }

  arrow::Status PartStream::Write(const void* data, std::int64_t nbytes)
  {
    const auto* p = static_cast<const std::uint8_t*>(data);
    buf_.insert(buf_.end(), p, p + nbytes);
    pos_ += nbytes;
    return arrow::Status::OK();
  }

  void PartStream::drain()
  {
    // the part's own leading magic is not copied: the joined file has one
    std::size_t skip = begin_ < 4 ? static_cast<std::size_t>(std::min<std::int64_t>(4 - begin_, static_cast<std::int64_t>(buf_.size()))) : 0;
    joiner_.append(part_, begin_ + static_cast<std::int64_t>(skip), buf_.data() + skip, static_cast<std::int64_t>(buf_.size() - skip));
    begin_ = pos_;
    buf_.clear();
  }

  arrow::Status PartStream::Close()
  {
    if (closed_) { return arrow::Status::OK(); }
    closed_ = true;
    try
    {
      // the tail is: remaining row groups (and page indexes), footer, footer length, "PAR1"
      if (buf_.size() < 8 || std::memcmp(buf_.data() + buf_.size() - 4, "PAR1", 4) != 0) { return arrow::Status::IOError("Parquet part without footer"); }
      std::uint32_t len = 0;
      for (int k = 3; k >= 0; --k) { len = len << 8 | buf_[buf_.size() - 8 + static_cast<std::size_t>(k)]; }
      if (len + 8 > buf_.size()) { return arrow::Status::IOError("Parquet part footer too long"); }
      std::string footer(reinterpret_cast<const char*>(buf_.data() + buf_.size() - 8 - len), len);
      buf_.resize(buf_.size() - 8 - len);
      pos_ = begin_ + static_cast<std::int64_t>(buf_.size());
      drain();
      joiner_.setFooter(part_, std::move(footer));
    }
    catch (const std::exception& e) { return arrow::Status::IOError(e.what()); }
    return arrow::Status::OK();
  }
}
