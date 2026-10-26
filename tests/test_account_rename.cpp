// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Renaming accounts (src/settings/account_rename.*): the name rules, the
// request files, the files that would give a new name rights it should not
// have, the checks, the edits of the files that name an account and, as
// root with usermod and groupmod installed, whole renames in a fake root
// (usermod --prefix): every file changed, then a failure at every step
// undone to the byte, and a rename interrupted by a "crash" undone at the
// next start. With the helper's path (argv[1]) also the helper: schedule,
// status, cancel, apply and its refusals. Never touches this computer's
// accounts: everything happens below a temporary directory.
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "account_rename.h"
#include "util.h"

using namespace sde;
using namespace sde::account_rename;

static int g_failures = 0;
static std::string g_tmp;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

static std::string text_of(const std::string &s) { return s; }
template <class T>
static std::string text_of(const T &v) {
  return std::to_string(v);
}
#define CHECK_EQ(a, b)                                                                              \
  do {                                                                                              \
    auto va = (a);                                                                                  \
    auto vb = (b);                                                                                  \
    if (!(va == vb)) {                                                                              \
      fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s\n  got \"%s\"\n", __FILE__, __LINE__, #a, #b, \
              text_of(va).c_str());                                                                 \
      ++g_failures;                                                                                 \
    }                                                                                               \
  } while (0)

// ------------------------------------------------------------ fake roots

void put(const std::string &path, const std::string &text, mode_t mode = 0644) {
  make_dirs(path.substr(0, path.rfind('/')));
  unlink(path.c_str());
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (fd < 0 || write(fd, text.data(), text.size()) != ssize_t(text.size())) {
    fprintf(stderr, "cannot write %s\n", path.c_str());
    abort();
  }
  fchmod(fd, mode);
  close(fd);
}

std::string get(const std::string &path) {
  std::string s;
  read_file(path, s);
  return s;
}

int sh(const std::string &cmd) {
  int st = system(cmd.c_str());
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// A system with the account sam (uid 1001) and the files that name it.
std::string make_root(const std::string &name) {
  std::string r = g_tmp + "/" + name;
  sh("rm -rf '" + r + "'");
  put(r + "/etc/passwd",
      "root:x:0:0:root:/root:/bin/bash\n"
      "sam:x:1001:1001:Sam Example,,,:/home/sam:/bin/bash\n"
      "mia:x:1002:1002:Mia:/home/mia:/bin/bash\n");
  put(r + "/etc/shadow", "root:*:19000:0:99999:7:::\nsam:$y$j9T$abc$def:19000:0:99999:7:::\nmia:!:19000:0:99999:7:::\n",
      0640);
  put(r + "/etc/group", "root:x:0:\nsudo:x:27:sam,mia\nsam:x:1001:\nmia:x:1002:\naudio:x:29:sam\n");
  put(r + "/etc/gshadow", "root:*::\nsudo:*::sam,mia\nsam:!::\nmia:!::\naudio:*::sam\n", 0640);
  put(r + "/home/sam/Documents/note.txt", "hello\n");
  put(r + "/home/mia/.profile", "# mia\n");
  put(r + "/var/lib/AccountsService/users/sam",
      "[User]\nLanguage=\nXSession=sde\nIcon=/var/lib/AccountsService/icons/sam\nSystemAccount=false\n", 0600);
  put(r + "/var/lib/AccountsService/icons/sam", "PNG", 0644);
  put(r + "/var/lib/AccountsService/users/mia", "[User]\nIcon=/var/lib/AccountsService/icons/mia\n", 0600);
  put(r + "/etc/lightdm/lightdm.conf", "[Seat:*]\n#autologin-user=\nautologin-user = sam\nautologin-user-timeout=0\n");
  put(r + "/etc/lightdm/lightdm.conf.d/50-sde.conf", "[Seat:*]\nuser-session=sde\nautologin-user=mia\n");
  put(r + "/etc/gdm3/daemon.conf", "[daemon]\nAutomaticLoginEnable=true\nAutomaticLogin=sam\n");
  put(r + "/var/spool/cron/crontabs/sam", "0 * * * * echo hi\n", 0600);
  put(r + "/var/mail/sam", "From someone\n", 0660);
  put(r + "/etc/subuid", "mia:100000:65536\nsam:165536:65536\n");
  put(r + "/etc/subgid", "mia:100000:65536\nsam:165536:65536\n");
  put(r + "/etc/sudoers.d/sam-tools", "# sam's tools\nsam ALL=(ALL) NOPASSWD: /home/sam/bin/backup\nDefaults:sam !lecture\n",
      0440);
  put(r + "/etc/sudoers.d/README", "# sam is not a rule here\n", 0440);
  put(r + "/var/lib/systemd/linger/sam", "", 0644);
  return r;
}

// Everything below |root|: paths with their kind, mode, owner and contents.
std::string snapshot(const std::string &root) {
  std::string out;
  std::function<void(const std::string &)> walk = [&](const std::string &rel) {
    for (const auto &n : list_dir(root + rel)) {
      std::string p = rel + "/" + n;
      struct stat st;
      if (lstat((root + p).c_str(), &st) != 0) continue;
      if (p == "/var/lib/sde") continue;  // the requests and journals
      // The backups the shadow tools keep (passwd- ...) and their lock.
      if ((rel == "/etc" && n.back() == '-') || p == "/etc/.pwd.lock") continue;
      char line[256];
      snprintf(line, sizeof line, " %o %u:%u ", unsigned(st.st_mode), unsigned(st.st_uid), unsigned(st.st_gid));
      out += p + line;
      if (S_ISDIR(st.st_mode)) {
        out += "dir\n";
        walk(p);
      } else if (S_ISLNK(st.st_mode)) {
        char t[512] = {0};
        ssize_t k = readlink((root + p).c_str(), t, sizeof t - 1);
        out += "-> " + std::string(t, size_t(k > 0 ? k : 0)) + "\n";
      } else {
        out += get(root + p) + "\n";
      }
    }
  };
  walk("");
  return out;
}

// --------------------------------------------------------------- tests

void test_names() {
  CHECK_EQ(login_name_problem("samuel"), std::string());
  CHECK_EQ(login_name_problem("s-a_m2"), std::string());
  CHECK(!login_name_problem("").empty());
  CHECK(!login_name_problem("Sam").empty());
  CHECK(!login_name_problem("2sam").empty());
  CHECK(!login_name_problem("sa m").empty());
  CHECK(!login_name_problem("sam.x").empty());
  CHECK(!login_name_problem(std::string(33, 'a')).empty());
  CHECK(login_name_problem(std::string(32, 'a')).empty());
  CHECK(!login_name_problem("root").empty());
  CHECK(!login_name_problem("sudo").empty());
  CHECK(!login_name_problem("lightdm").empty());
}

void test_requests() {
  Request r;
  r.uid = 1001;
  r.old_name = "sam";
  r.new_name = "samuel";
  r.old_home = "/home/sam";
  r.new_home = "/home/samuel";
  r.link = true;
  r.by = 1000;
  r.time = 1790000000;
  Request back;
  std::string error;
  CHECK(parse_request(format_request(r), &back, &error));
  CHECK(back.uid == 1001 && back.old_name == "sam" && back.new_name == "samuel" && back.link && back.by == 1000);
  CHECK(back.old_home == "/home/sam" && back.new_home == "/home/samuel" && back.state == "pending");
  r.state = "failed";
  r.error = "usermod: no\nway";
  CHECK(parse_request(format_request(r), &back, &error));
  CHECK_EQ(back.error, std::string("usermod: no way"));
  auto bad = [&](const std::string &text) {
    Request x;
    std::string e;
    return !parse_request(text, &x, &e) && !e.empty();
  };
  std::string good = "uid=1001\nold=sam\nnew=samuel\nold_home=/home/sam\nnew_home=/home/samuel\n";
  CHECK(!bad(good));
  CHECK(bad(good + "shell=/bin/sh\n"));                                     // unknown key
  CHECK(bad(good + "uid=1002\n"));                                          // twice
  CHECK(bad("uid=x\nold=sam\nnew=samuel\nold_home=/home/sam\nnew_home=/home/samuel\n"));
  CHECK(bad("uid=1001\nold=sam\nnew=Samuel\nold_home=/home/sam\nnew_home=/home/Samuel\n"));
  CHECK(bad("uid=1001\nold=sam\nnew=root\nold_home=/home/sam\nnew_home=/home/root\n"));
  CHECK(bad("uid=1001\nold=sam\nnew=sam\nold_home=/home/sam\nnew_home=/home/sam\n"));
  CHECK(bad("uid=1001\nold=sam\nnew=samuel\nold_home=/home/sam\nnew_home=/etc\n"));  // not the folder renamed
  CHECK(bad("uid=1001\nold=sam\nnew=samuel\nold_home=home/sam\nnew_home=home/samuel\n"));
  CHECK(bad("uid=1001\nold=sam\nnew=samuel\nold_home=/home/../sam\nnew_home=/home/../samuel\n"));
  CHECK(bad(good + "state=done\n"));
  CHECK(bad(good + "error=a\tb\n"));
  CHECK(bad("uid=1001\nold=-rf\nnew=samuel\nold_home=/home/x\nnew_home=/home/x\n"));
  Request same;
  CHECK(parse_request("uid=1001\nold=sam\nnew=samuel\nold_home=/srv/data\nnew_home=/srv/data\n", &same, &error));

  std::string dir = g_tmp + "/requests";
  make_dirs(dir);
  r.state = "pending";
  r.error.clear();
  CHECK(save_request(dir, r, &error));
  struct stat st;
  CHECK(stat(request_path(dir, 1001).c_str(), &st) == 0 && (st.st_mode & 0777) == 0644);
  put(dir + "/1002.rename", "garbage\n");
  put(dir + "/1003.rename", "uid=1004\nold=x\nnew=y\nold_home=/home/x\nnew_home=/home/y\n");  // wrong file name
  std::vector<Request> all = load_requests(dir);
  CHECK(all.size() == 1 && all[0].uid == 1001);

  CHECK_EQ(new_home_for("/home/sam", "sam", "samuel"), std::string("/home/samuel"));
  CHECK_EQ(new_home_for("/home/sam/", "sam", "samuel"), std::string("/home/samuel"));
  CHECK_EQ(new_home_for("/sam", "sam", "samuel"), std::string("/samuel"));
  CHECK_EQ(new_home_for("/srv/users/sam-data", "sam", "samuel"), std::string("/srv/users/sam-data"));
  CHECK_EQ(new_home_for("/nonexistent", "sam", "samuel"), std::string("/nonexistent"));
  setenv("SDE_ACCOUNT_RENAME_DIR", "/x/y", 1);
  CHECK_EQ(default_dir(), std::string("/x/y"));
  unsetenv("SDE_ACCOUNT_RENAME_DIR");
  CHECK_EQ(default_dir(), std::string("/var/lib/sde/account-renames"));
}

void test_mentions_and_checks() {
  std::string r = make_root("checks");
  put(r + "/etc/group", get(r + "/etc/group") + "plugdev:x:46:bob,sam\n");
  put(r + "/etc/gshadow", get(r + "/etc/gshadow") + "plugdev:*:bobadmin:\n");
  put(r + "/etc/sudoers",
      "Defaults env_reset\nDefaults secure_path=\"/usr/local/sbin:/usr/sbin:/snap/bin\"\n"
      "root ALL=(ALL:ALL) ALL\n%sudo ALL=(ALL:ALL) ALL\n# nina ALL=(ALL) ALL\n"
      "#includedir /etc/sudoers.d\n", 0440);
  put(r + "/etc/sudoers.d/90-people",
      "carol ALL=(ALL) ALL\nDefaults:dave !authenticate\nUser_Alias ADMINS = erin, \\\n  frank\n"
      "Cmnd_Alias TOOLS = /usr/bin/gina\nmia ALL=(hank) /usr/bin/id\n", 0440);
  put(r + "/etc/sudoers.d/ignored.conf", "oscar ALL=(ALL) ALL\n", 0440);  // sudo skips names with a dot
  put(r + "/etc/polkit-1/rules.d/50-ivan.rules",
      "// paula is in a comment\npolkit.addRule(function(action, subject) {\n"
      "  if (subject.user == \"ivan\") return polkit.Result.YES;\n});\n");
  put(r + "/etc/polkit-1/localauthority/50-local.d/judy.pkla", "[x]\nIdentity=unix-user:judy\nResultAny=yes\n");
  put(r + "/etc/ssh/sshd_config", "PermitRootLogin no\nAllowUsers kate mia\n");
  put(r + "/etc/security/access.conf", "# - : quinn : ALL\n+ : leo : ALL\n");
  put(r + "/etc/cron.allow", "mike\n");
  auto named = [&](const std::string &name) { return mentions(r, name); };
  CHECK(named("bob").size() == 1 && named("bob")[0] == "/etc/group");
  CHECK(named("bobadmin").size() == 1 && named("bobadmin")[0] == "/etc/gshadow");
  CHECK(named("carol").size() == 1 && named("carol")[0] == "/etc/sudoers.d/90-people");
  CHECK(named("dave").size() == 1);
  CHECK(named("erin").size() == 1);
  CHECK(named("frank").size() == 1);  // a continued line
  CHECK(named("gina").empty());       // only a command
  CHECK(named("hank").size() == 1);   // run as
  CHECK(named("ivan").size() == 1 && named("ivan")[0] == "/etc/polkit-1/rules.d/50-ivan.rules");
  CHECK(named("paula").empty());
  CHECK(named("judy").size() == 1);
  CHECK(named("kate").size() == 1 && named("kate")[0] == "/etc/ssh/sshd_config");
  CHECK(named("leo").size() == 1 && named("leo")[0] == "/etc/security/access.conf");
  CHECK(named("quinn").empty());
  CHECK(named("mike").size() == 1 && named("mike")[0] == "/etc/cron.allow");
  CHECK(named("nina").empty());   // a comment
  CHECK(named("oscar").empty());  // a file sudo does not read
  CHECK(named("snap").empty());   // a Defaults value
  CHECK(named("samuel").empty());

  std::vector<Passwd> users = read_passwd(r);
  CHECK(users.size() == 3 && users[1].name == "sam" && users[1].uid == 1001 && users[1].home == "/home/sam");
  std::vector<Group> groups = read_group(r);
  CHECK(groups.size() == 6 && groups[1].name == "sudo" && groups[1].members.size() == 2);
  uid_t min = 0, max = 0;
  uid_range(r, &min, &max);
  CHECK(min == 1000 && max == 60000);
  put(r + "/etc/login.defs", "# defaults\nUID_MIN\t\t\t 2000\nUID_MAX 3000\n");
  uid_range(r, &min, &max);
  CHECK(min == 2000 && max == 3000);
  CHECK(!check(r, 1001, "sam", "samuel", {}).empty());  // below UID_MIN now
  unlink((r + "/etc/login.defs").c_str());

  CHECK_EQ(check(r, 1001, "sam", "samuel", {}), std::string());
  CHECK(!check(r, 1001, "sam", "sam", {}).empty());
  CHECK(!check(r, 1001, "sam", "mia", {}).empty());     // a user
  CHECK(!check(r, 1001, "sam", "audio", {}).empty());   // reserved
  CHECK(!check(r, 1001, "sam", "plugdev", {}).empty()); // reserved
  put(r + "/etc/group", get(r + "/etc/group") + "scanner:x:120:\n");
  CHECK(!check(r, 1001, "sam", "scanner", {}).empty()); // a group
  CHECK(!check(r, 1001, "sam", "carol", {}).empty());   // sudo rules name it
  CHECK(check(r, 1001, "sam", "carol", {}).find("90-people") != std::string::npos);
  CHECK(!check(r, 1001, "sam", "bob", {}).empty());     // a member of plugdev
  CHECK(!check(r, 1001, "samx", "samuel", {}).empty()); // the account is called sam
  CHECK(!check(r, 1999, "nobody2", "samuel", {}).empty());
  CHECK(!check(r, 0, "root", "superuser", {}).empty()); // not a person's account
  Request other;
  other.uid = 1002;
  other.old_name = "mia";
  other.new_name = "samuel";
  CHECK(!check(r, 1001, "sam", "samuel", {other}).empty());
  other.new_name = "maria";
  CHECK(check(r, 1001, "sam", "mia2", {other}).empty());
  Request mine = other;
  mine.uid = 1001;
  mine.new_name = "samuel";
  CHECK(check(r, 1001, "sam", "samuel", {mine}).empty());  // its own earlier request
  make_dirs(r + "/home/samuel");
  CHECK(!check(r, 1001, "sam", "samuel", {}).empty());  // the folder exists
}

void test_edits() {
  CHECK_EQ(edit_lightdm("[Seat:*]\nautologin-user=sam\n", "sam", "samuel"), std::string("[Seat:*]\nautologin-user=samuel\n"));
  CHECK_EQ(edit_lightdm("autologin-user = sam  \n", "sam", "samuel"), std::string("autologin-user = samuel  \n"));
  CHECK_EQ(edit_lightdm("autologin-user=samantha\n", "sam", "samuel"), std::string("autologin-user=samantha\n"));
  CHECK_EQ(edit_lightdm("#autologin-user=sam\n", "sam", "samuel"), std::string("#autologin-user=sam\n"));
  CHECK_EQ(edit_lightdm("autologin-user-timeout=sam\n", "sam", "samuel"), std::string("autologin-user-timeout=sam\n"));
  CHECK_EQ(edit_lightdm("no newline autologin-user=sam", "sam", "samuel"), std::string("no newline autologin-user=sam"));
  CHECK_EQ(edit_gdm("[daemon]\nAutomaticLogin=sam\nTimedLogin=sam\nAutomaticLoginEnable=true\n", "sam", "samuel"),
           std::string("[daemon]\nAutomaticLogin=samuel\nTimedLogin=samuel\nAutomaticLoginEnable=true\n"));
  CHECK_EQ(edit_subid("mia:100000:65536\nsam:165536:65536\nsamantha:1:2\n", "sam", "samuel"),
           std::string("mia:100000:65536\nsamuel:165536:65536\nsamantha:1:2\n"));
  CHECK_EQ(edit_sudoers("# sam\nsam ALL=(sam) /home/sam/bin/x\n%sam ALL=(ALL) ALL\nsamantha ALL=ALL\n", "sam", "samuel"),
           std::string("# sam\nsamuel ALL=(samuel) /home/samuel/bin/x\n%samuel ALL=(ALL) ALL\nsamantha ALL=ALL\n"));
  CHECK_EQ(edit_sudoers("#includedir /etc/sudoers.d/sam\n", "sam", "samuel"), std::string("#includedir /etc/sudoers.d/sam\n"));
  CHECK_EQ(edit_accountsservice("[User]\nIcon=/var/lib/AccountsService/icons/sam\n", "/var/lib/AccountsService/icons/sam",
                                "/var/lib/AccountsService/icons/samuel"),
           std::string("[User]\nIcon=/var/lib/AccountsService/icons/samuel\n"));
}

// --------------------------------------------------- renames (as root)

Request sam_request(bool link = false) {
  Request r;
  r.uid = 1001;
  r.old_name = "sam";
  r.new_name = "samuel";
  r.old_home = "/home/sam";
  r.new_home = "/home/samuel";
  r.link = link;
  return r;
}

void test_apply() {
  std::string root = make_root("apply");
  std::string dir = root + "/var/lib/sde/account-renames";
  make_dirs(dir);
  std::vector<std::string> logged;
  Applier a(root, dir);
  a.log = [&](const std::string &l) { logged.push_back(l); };
  std::string error;
  bool postponed = true;
  CHECK(a.apply(sam_request(true), &error, &postponed));
  if (!error.empty()) fprintf(stderr, "apply: %s\n", error.c_str());
  CHECK(!postponed);
  std::string passwd = get(root + "/etc/passwd");
  CHECK(passwd.find("samuel:x:1001:1001:Sam Example,,,:/home/samuel:/bin/bash") != std::string::npos);
  CHECK(passwd.find("\nsam:") == std::string::npos);
  CHECK(get(root + "/etc/shadow").find("samuel:$y$j9T$abc$def:") != std::string::npos);
  std::string group = get(root + "/etc/group");
  CHECK(group.find("sudo:x:27:") != std::string::npos && group.find("samuel") != std::string::npos);
  CHECK(group.find("samuel:x:1001:") != std::string::npos && group.find("\nsam:x") == std::string::npos);
  CHECK(group.find("audio:x:29:samuel") != std::string::npos);
  CHECK(get(root + "/etc/gshadow").find("samuel:!::") != std::string::npos);
  CHECK_EQ(get(root + "/home/samuel/Documents/note.txt"), std::string("hello\n"));
  struct stat st;
  CHECK(lstat((root + "/home/sam").c_str(), &st) == 0 && S_ISLNK(st.st_mode));  // the link asked for
  char target[64] = {0};
  CHECK(readlink((root + "/home/sam").c_str(), target, sizeof target - 1) > 0 && std::string(target) == "samuel");
  CHECK(file_exists(root + "/var/lib/AccountsService/users/samuel") &&
        !file_exists(root + "/var/lib/AccountsService/users/sam"));
  CHECK(get(root + "/var/lib/AccountsService/users/samuel").find("Icon=/var/lib/AccountsService/icons/samuel\n") !=
        std::string::npos);
  CHECK(stat((root + "/var/lib/AccountsService/users/samuel").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
  CHECK(file_exists(root + "/var/lib/AccountsService/icons/samuel"));
  CHECK_EQ(get(root + "/var/lib/AccountsService/users/mia"), std::string("[User]\nIcon=/var/lib/AccountsService/icons/mia\n"));
  CHECK(get(root + "/etc/lightdm/lightdm.conf").find("autologin-user = samuel\n") != std::string::npos);
  CHECK(get(root + "/etc/lightdm/lightdm.conf.d/50-sde.conf").find("autologin-user=mia\n") != std::string::npos);
  CHECK(get(root + "/etc/gdm3/daemon.conf").find("AutomaticLogin=samuel\n") != std::string::npos);
  CHECK(file_exists(root + "/var/spool/cron/crontabs/samuel") && !file_exists(root + "/var/spool/cron/crontabs/sam"));
  CHECK(file_exists(root + "/var/mail/samuel") && !file_exists(root + "/var/mail/sam"));
  CHECK_EQ(get(root + "/etc/subuid"), std::string("mia:100000:65536\nsamuel:165536:65536\n"));
  CHECK_EQ(get(root + "/etc/subgid"), std::string("mia:100000:65536\nsamuel:165536:65536\n"));
  CHECK_EQ(get(root + "/etc/sudoers.d/sam-tools"),
           std::string("# sam's tools\nsamuel ALL=(ALL) NOPASSWD: /home/samuel/bin/backup\nDefaults:samuel !lecture\n"));
  CHECK(stat((root + "/etc/sudoers.d/sam-tools").c_str(), &st) == 0 && (st.st_mode & 0777) == 0440);
  CHECK_EQ(get(root + "/etc/sudoers.d/README"), std::string("# sam is not a rule here\n"));
  CHECK(file_exists(root + "/var/lib/systemd/linger/samuel") && !file_exists(root + "/var/lib/systemd/linger/sam"));
  CHECK(!file_exists(dir + "/1001.journal") && !dir_exists(dir + "/1001.backup"));
  bool said = false;
  for (const auto &l : logged)
    if (l == "renamed sam to samuel") said = true;
  CHECK(said);
  // Once more: sam does not exist any more.
  CHECK(!a.apply(sam_request(), &error));
}

void test_rollback() {
  const char *steps[] = {"usermod", "groupmod", "accountsservice", "lightdm", "gdm", "crontab", "mail",
                         "subuid",  "subgid",   "sudoers",         "linger",  "link", "end"};
  for (const char *s : steps) {
    std::string root = make_root(std::string("rollback-") + s);
    std::string dir = root + "/var/lib/sde/account-renames";
    make_dirs(dir);
    std::string before = snapshot(root);
    Applier a(root, dir);
    a.fail_at = s;
    std::string error;
    bool ok = a.apply(sam_request(true), &error);
    CHECK(!ok);
    if (error.find(s) == std::string::npos) fprintf(stderr, "rollback %s: %s\n", s, error.c_str());
    CHECK(error.find(s) != std::string::npos);
    std::string after = snapshot(root);
    if (after != before) fprintf(stderr, "rollback at %s left changes:\n--- before\n%s--- after\n%s", s, before.c_str(),
                                 after.c_str());
    CHECK(after == before);
    CHECK(!file_exists(dir + "/1001.journal"));
  }
}

void test_recover() {
  const char *points[] = {"usermod", "groupmod", "accountsservice", "mail", "sudoers", "link"};
  for (const char *p : points) {
    std::string root = make_root(std::string("crash-") + p);
    std::string dir = root + "/var/lib/sde/account-renames";
    make_dirs(dir);
    std::string before = snapshot(root);
    {
      Applier a(root, dir);
      a.crash_after = p;
      std::string error;
      CHECK(!a.apply(sam_request(true), &error));
    }
    CHECK(file_exists(dir + "/1001.journal"));
    CHECK(snapshot(root) != before);
    Applier b(root, dir);
    std::string error;
    CHECK(b.recover(1001, &error));
    std::string after = snapshot(root);
    if (after != before) fprintf(stderr, "recover after %s left changes:\n%s\n---\n%s", p, before.c_str(), after.c_str());
    CHECK(after == before);
    CHECK(!file_exists(dir + "/1001.journal") && !dir_exists(dir + "/1001.backup"));
  }
  // A journal that ended: the rename was made, nothing is undone.
  std::string root = make_root("crash-ended");
  std::string dir = root + "/var/lib/sde/account-renames";
  make_dirs(dir);
  put(dir + "/1001.journal", "begin\t1001\tsam\tsamuel\tnow\nmove\t/var/mail/sam\t/var/mail/samuel\nend\n", 0600);
  std::string before = snapshot(root);
  Applier c(root, dir);
  std::string error;
  CHECK(c.recover(1001, &error));
  CHECK(snapshot(root) == before);
}

// ----------------------------------------------------------- the helper

int helper(const std::string &program, const std::vector<std::string> &args, std::string *out,
           const char *pkexec_uid = nullptr) {
  std::string cmd;
  if (pkexec_uid) cmd += "PKEXEC_UID=" + std::string(pkexec_uid) + " ";
  cmd += "'" + program + "'";
  for (const auto &a : args) cmd += " '" + a + "'";
  cmd += " > '" + g_tmp + "/helper.out' 2>&1";
  int st = sh(cmd);
  *out = get(g_tmp + "/helper.out");
  return st;
}

void test_helper(const std::string &program) {
  std::string root = make_root("helper");
  std::string dir = root + "/var/lib/sde/account-renames";
  std::string out;
  CHECK(helper(program, {"--version"}, &out) == 0 && out.find("sde-account-rename") == 0);
  CHECK(helper(program, {"bogus"}, &out) == 2);
  CHECK(helper(program, {"schedule", "--uid", "1001", "--old", "sam"}, &out) == 2);  // no new name
  // pkexec callers cannot point it elsewhere.
  CHECK(helper(program, {"schedule-self", "--uid", "1001", "--old", "sam", "--new", "samuel", "--dir", dir}, &out,
               "1001") == 1);
  CHECK(out.find("ERROR usage") == 0);
  // Refused names.
  CHECK(helper(program, {"schedule", "--uid", "1001", "--old", "sam", "--new", "mia", "--root", root, "--dir", dir},
               &out) == 1);
  CHECK(out.find("ERROR name ") == 0);
  CHECK(helper(program, {"schedule", "--uid", "1001", "--old", "sam", "--new", "Bad Name", "--root", root, "--dir",
                         dir},
               &out) == 1);
  // schedule-self names the caller's own account only (here: the uid running the test).
  std::string me = std::to_string(getuid() == 1001 ? 1002 : 1001);
  CHECK(helper(program, {"schedule-self", "--uid", me, "--old", "sam", "--new", "samuel", "--root", root, "--dir", dir},
               &out) == 1);
  CHECK(out.find("ERROR self") == 0);
  // Scheduled, shown, cancelled, scheduled again.
  CHECK(helper(program, {"schedule", "--uid", "1001", "--old", "sam", "--new", "samuel", "--link", "--root", root,
                         "--dir", dir},
               &out) == 0);
  CHECK(out.find("OK scheduled sam samuel") == 0);
  Request r;
  std::string error;
  CHECK(parse_request(get(request_path(dir, 1001)), &r, &error));
  CHECK(r.new_name == "samuel" && r.new_home == "/home/samuel" && r.link && r.by == getuid());
  CHECK(helper(program, {"status", "--dir", dir}, &out) == 0);
  CHECK(out.find("pending uid=1001 old=sam new=samuel home=/home/samuel link=1") == 0);
  // Another account cannot ask for the same name.
  CHECK(helper(program, {"schedule", "--uid", "1002", "--old", "mia", "--new", "samuel", "--root", root, "--dir", dir},
               &out) == 1);
  CHECK(helper(program, {"cancel", "--uid", "1001", "--dir", dir}, &out) == 0 && out.find("OK cancelled") == 0);
  CHECK(!file_exists(request_path(dir, 1001)));
  CHECK(helper(program, {"cancel", "--uid", "1001", "--dir", dir}, &out) == 0);
  CHECK(helper(program, {"schedule", "--uid", "1001", "--old", "sam", "--new", "samuel", "--root", root, "--dir", dir},
               &out) == 0);
  CHECK(get(dir + "/rename.log").find("scheduled: sam (uid 1001) to samuel") != std::string::npos);
  if (getuid() != 0) return;
  // apply, as the boot service does.
  CHECK(helper(program, {"apply", "--root", root, "--dir", dir}, &out) == 0);
  CHECK(out.find("OK renamed sam samuel") != std::string::npos);
  CHECK(!file_exists(request_path(dir, 1001)));
  CHECK(get(root + "/etc/passwd").find("samuel:x:1001:") != std::string::npos);
  CHECK(dir_exists(root + "/home/samuel") && !file_exists(root + "/home/sam"));
  // A failure is recorded in the request, which waits for the user.
  CHECK(helper(program, {"schedule", "--uid", "1002", "--old", "mia", "--new", "maria", "--root", root, "--dir", dir},
               &out) == 0);
  std::string before = snapshot(root);
  CHECK(helper(program, {"apply", "--root", root, "--dir", dir, "--fail-at", "lightdm"}, &out) == 1);
  CHECK(snapshot(root) == before);
  CHECK(parse_request(get(request_path(dir, 1002)), &r, &error) && r.state == "failed" &&
        r.error.find("lightdm") != std::string::npos);
  CHECK(helper(program, {"apply", "--root", root, "--dir", dir}, &out) == 0);  // failed ones wait
  CHECK(get(root + "/etc/passwd").find("mia:x:1002") != std::string::npos);
  // A directory others may write to is not trusted.
  chmod(dir.c_str(), 0777);
  CHECK(helper(program, {"apply", "--root", root, "--dir", dir}, &out) == 1 && out.find("ERROR dir") == 0);
  chmod(dir.c_str(), 0755);
}

int main(int argc, char **argv) {
  char tmpl[] = "/tmp/sde-rename-test-XXXXXX";
  char *dir = mkdtemp(tmpl);
  if (!dir) return 2;
  g_tmp = dir;
  test_names();
  test_requests();
  test_mentions_and_checks();
  test_edits();
  bool tools = !find_program("usermod").empty() || file_exists("/usr/sbin/usermod");
  if (getuid() == 0 && tools) {
    test_apply();
    test_rollback();
    test_recover();
  } else {
    printf("account-rename-tests: renames in a fake root skipped (they need root and usermod)\n");
  }
  if (argc > 1) test_helper(argv[1]);
  sh("rm -rf '" + g_tmp + "'");
  if (g_failures) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("account-rename-tests: all passed\n");
  return 0;
}
