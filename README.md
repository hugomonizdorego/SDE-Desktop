# Selviana Desktop Environment (SDE)

**A small, fast, and modern desktop environment**

Selviana Desktop Environment (SDE) is an EDE-based desktop project designed for Debian and Debian-based Linux distributions. It aims to deliver a responsive desktop experience, low resource usage, and a familiar look and feel with a clean, modern interface.

Versi paket fondasi: **0.1.0-alpha.1** · snapshot upstream: **16 Oktober 2026**.

SDE adalah proyek fork independen yang direncanakan dari **Equinox Desktop Environment (EDE)** untuk melanjutkan pengembangan desktop yang kecil, cepat, dan mudah digunakan. Nama lengkap yang digunakan adalah **Selviana Desktop Environment**.

Paket ini menggabungkan source tree dari **seluruh 11 repositori publik `edeproject`** ke **satu monorepo**, tanpa submodule dan tanpa direktori `.git` bersarang. Enam repositori yang diminta semuanya disertakan; lima repositori tambahan melengkapi sumber situs, artwork, tema, file manager, dan installer lama.

## Status yang sebenarnya

Ini adalah **fondasi source untuk memulai SDE**, bukan rilis desktop modern yang telah selesai. Source upstream dipertahankan utuh. Aplikasi, binary, konfigurasi, dan sesi di dalam source masih menggunakan nama EDE. Penggantian nama runtime dan desain GUI adalah pekerjaan berikutnya; lihat [roadmap](docs/ROADMAP.md).

Paket berisi semua berkas yang dilacak Git pada commit branch default masing-masing repositori. Riwayat objek Git, isi branch/tag lain yang berbeda, issue/PR GitHub, release assets, dan database situs yang sedang berjalan tidak disertakan. Daftar refs dan commit asal tetap dicatat. FLTK, Xorg, compiler, dan dependency sistem diperoleh dari distribusi, bukan dibundel.

## Struktur satu proyek

| Sumber asli | Lokasi di monorepo | Peran |
| --- | --- | --- |
| `edeproject/ede` | `src/ede/` | Basis desktop aktif: panel, desktop, launcher, sesi, konfigurasi, pekwm |
| `edeproject/edelib` | `src/edelib/` | Library C++ pendukung desktop |
| `edeproject/jam` | `tools/jam/` | Build tool upstream |
| `edeproject/efltk` | `legacy/efltk/` | Toolkit generasi lama; referensi |
| `edeproject/ede-1.x` | `legacy/ede-1.x/` | EDE generasi pertama; referensi |
| `edeproject/svn` | `legacy/svn/` | Source tree arsip SVN, termasuk trunk/tags/branches di dalam tree tersebut |
| `edeproject/themes-1.x` | `legacy/themes-1.x/` | Tema generasi lama |
| `edeproject/net-install` | `legacy/net-install/` | Installer jaringan lama; tidak dijalankan otomatis |
| `edeproject/efiler` | `extras/efiler/` | Kandidat file manager; perlu evaluasi |
| `edeproject/art` | `extras/art/` | Artwork asli untuk referensi; periksa izin sebelum penggunaan ulang |
| `edeproject/www` | `reference/www/` | Source situs dan data yang tersimpan di Git; bukan salinan seluruh situs live |

Folder komponen dipertahankan agar nama berkas dan sistem build tidak bertabrakan. Semuanya tetap berada dalam satu repository GitHub, bukan digabung secara paksa menjadi satu file C++.

## Mulai di sini

1. [Cara upload melalui browser](docs/BROWSER-UPLOAD.md) atau [menggunakan Git](docs/GITHUB.md).
2. [Dependency dan percobaan build](docs/BUILD.md).
3. [Roadmap 0.1 Alpha sampai 1.0](docs/ROADMAP.md).
4. [Arah desain antarmuka](docs/DESIGN.md).
5. [Panduan bekerja dengan AI](docs/AI-DEVELOPMENT.md) dan `AGENTS.md`.
6. [Lisensi dan atribusi](LICENSE.md), [catatan pihak ketiga](docs/THIRD-PARTY.md).
7. [Hasil pemeriksaan paket](docs/VALIDATION.md).

Perintah awal, dari folder ini:

```sh
python3 scripts/verify-source.py
python3 scripts/sde.py doctor
python3 scripts/sde.py build-jam
```

`doctor` akan mengembalikan status gagal jika dependency belum tersedia. `build-jam` hanya membangun Jam dalam `.sde-work/`. Percobaan build desktop: `python3 scripts/sde.py build`; baca batas validasinya di BUILD.md. Script tidak meminta sudo dan tidak mengubah desktop sistem.

## Asal-usul dan identitas

Upstream: [EDE di GitHub](https://github.com/edeproject) · [situs EDE](https://edeproject.org/).

Semua 25.653 berkas upstream dicocokkan dengan blob Git asal dan dicatat dengan SHA-256 di `provenance/upstream-files.json`. Commit dan daftar refs tersimpan di `provenance/upstream-lock.json`. Penambahan SDE dijelaskan di CHANGELOG.md.

SDE adalah fork independen; penggabungan kode tidak memindahkan kepemilikan organisasi, repository, merek, atau hak cipta EDE. Kredit penulis EDE, edelib, pekwm, FLTK, dan komponen pihak ketiga tetap dipertahankan sesuai pemberitahuan asal.
