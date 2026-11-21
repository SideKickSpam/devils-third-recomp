"""Read Unreal Engine 3 packages as the Wii U build of Devil's Third ships them (.xxx, big-endian, version 868).

As a module: load(path) returns the decompressed package with its name, import and export tables.
As a tool:
    python3 -I src/tools/ue3_package.py PACKAGE [--class CLASS] [--grep TEXT]
lists the package's exports (class, path, size), optionally only those of a class or whose path contains TEXT.

Packages come in two forms: "fully compressed" (a .uncompressed_size file beside them: the whole file is a run of
compressed chunks) and normal ones whose summary lists compressed chunks. Both use zlib. Package files are game data:
treat them as untrusted input and never commit them or anything extracted from them.
"""
import argparse
import os
import struct
import zlib

TAG = 0x9E2A83C1


class Reader:
    def __init__(self, data, offset=0):
        self.d, self.o = data, offset

    def i32(self):
        v = struct.unpack_from('>i', self.d, self.o)[0]
        self.o += 4
        return v

    def u32(self):
        v = struct.unpack_from('>I', self.d, self.o)[0]
        self.o += 4
        return v

    def i64(self):
        v = struct.unpack_from('>q', self.d, self.o)[0]
        self.o += 8
        return v

    def take(self, n):
        v = self.d[self.o:self.o + n]
        self.o += n
        return v

    def fstring(self):
        n = self.i32()
        if n == 0:
            return ''
        if n > 0:
            v = self.d[self.o:self.o + n - 1].decode('latin1')
            self.o += n
            return v
        n = -n
        v = self.d[self.o:self.o + 2 * (n - 1)].decode('utf-16-be', 'replace')
        self.o += 2 * n
        return v


def _chunk(r):
    """One compressed chunk at r: its zlib blocks, decompressed."""
    if r.u32() != TAG:
        raise ValueError('bad compressed chunk tag')
    block = r.i32()
    r.i32()  # compressed size
    total = r.i32()
    infos = [(r.i32(), r.i32()) for _ in range((total + block - 1) // block)]
    return b''.join(zlib.decompress(r.take(c)) for c, _ in infos)


def _fully_decompress(data):
    out, r = bytearray(), Reader(data)
    while r.o < len(data) and struct.unpack_from('>I', data, r.o)[0] == TAG:
        out += _chunk(r)
    return bytes(out)


class Package:
    def __init__(self, path):
        data = open(path, 'rb').read()
        if os.path.exists(path + '.uncompressed_size'):
            data = _fully_decompress(data)
        r = Reader(data)
        if r.u32() != TAG:
            raise ValueError(f'{path}: not an Unreal package')
        v = r.i32()
        self.version, self.licensee = v & 0xFFFF, v >> 16
        r.i32()  # header size
        self.folder = r.fstring()
        self.flags = r.u32()
        nc, no, ec, eo, ic, io = (r.i32() for _ in range(6))
        r.i32()  # depends offset
        if self.version >= 623:
            r.take(12)
        if self.version >= 584:
            r.i32()
        r.take(16)  # guid
        for _ in range(r.i32()):  # generations
            r.take(12)
        r.take(8)  # engine version, cooked content version
        compression = r.u32()
        chunks = [(r.i32(), r.i32(), r.i32(), r.i32()) for _ in range(r.i32())]
        if chunks:
            if compression != 1:
                raise ValueError(f'{path}: compression {compression} (only zlib is supported)')
            out = bytearray(data)
            for uoff, _, coff, _ in chunks:
                block = _chunk(Reader(data, coff))
                if len(out) < uoff + len(block):
                    out += bytes(uoff + len(block) - len(out))
                out[uoff:uoff + len(block)] = block
            data = bytes(out)
        self.data = data

        r = Reader(data, no)
        self.names = []
        for _ in range(nc):
            self.names.append(r.fstring())
            r.i64()

        def name(r):
            i, n = r.i32(), r.i32()
            return self.names[i] + (f'_{n - 1}' if n else '')

        r = Reader(data, io)
        self.imports = []
        for _ in range(ic):
            class_package, class_name, outer, obj = name(r), name(r), r.i32(), name(r)
            self.imports.append(dict(class_package=class_package, class_name=class_name, outer=outer, name=obj))

        r = Reader(data, eo)
        self.exports = []
        for _ in range(ec):
            cls, _super, outer = r.i32(), r.i32(), r.i32()
            obj = name(r)
            if self.version >= 220:
                r.i32()  # archetype
            r.i64()  # object flags
            size, offset = r.i32(), r.i32()
            if self.version < 543:
                for _ in range(r.i32()):
                    r.take(12)
            r.u32()  # export flags
            for _ in range(r.i32()):
                r.i32()
            r.take(20)  # package guid and flags
            self.exports.append(dict(cls=cls, outer=outer, name=obj, size=size, offset=offset))

    def object_name(self, index):
        """An object reference (export > 0, import < 0, 0 = class Class) as its name."""
        if index > 0:
            return self.exports[index - 1]['name']
        if index < 0:
            return self.imports[-index - 1]['name']
        return 'Class'

    def path(self, index):
        """An object's full path (outer.inner...)."""
        parts = []
        while index:
            parts.append(self.object_name(index))
            index = self.exports[index - 1]['outer'] if index > 0 else self.imports[-index - 1]['outer']
        return '.'.join(reversed(parts))

    def class_of(self, export):
        return self.object_name(export['cls'])

    def export_data(self, export):
        return self.data[export['offset']:export['offset'] + export['size']]


def load(path):
    return Package(path)


def main():
    ap = argparse.ArgumentParser(description='List the exports of an Unreal Engine 3 package.')
    ap.add_argument('package')
    ap.add_argument('--class', dest='cls', help='only exports of this class (e.g. SwfMovie)')
    ap.add_argument('--grep', help='only exports whose path contains this text')
    args = ap.parse_args()
    p = load(args.package)
    print(f'version {p.version}, licensee {p.licensee}: {len(p.names)} names, {len(p.imports)} imports, '
          f'{len(p.exports)} exports')
    for i, e in enumerate(p.exports):
        cls, path = p.class_of(e), p.path(i + 1)
        if (args.cls and cls != args.cls) or (args.grep and args.grep not in path):
            continue
        print(f'{cls:30s} {path:70s} {e["size"]}')


if __name__ == '__main__':
    main()
