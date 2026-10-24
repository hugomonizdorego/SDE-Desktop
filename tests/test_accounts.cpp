// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the user accounts of Control Center > Users & Avatar
// (src/settings/accounts.h): spelling user names, the rules for names and
// passwords, crypt(3) hashes, who may be removed, locked or demoted, the
// calls made to AccountsService (a fake backend) and passwd(1) on a
// pseudo-terminal (fake passwd scripts).
#include <crypt.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "accounts.h"
#include "util.h"

using namespace sde;
using namespace sde::accounts;

static int g_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

#define CHECK_EQ(a, b)                                                                     \
  do {                                                                                     \
    auto va = (a);                                                                         \
    auto vb = (b);                                                                         \
    if (!(va == vb)) {                                                                     \
      fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s\n  got \"%s\"\n", __FILE__, __LINE__, \
              #a, #b, std::string(va).c_str());                                            \
      ++g_failures;                                                                        \
    }                                                                                      \
  } while (0)

static void test_spelling() {
  CHECK_EQ(ascii_words("  Hugo  Moniz do Rego "), std::string("hugo moniz do rego"));
  CHECK_EQ(ascii_words("Élodie Ñúñez-Gómez"), std::string("elodie nunez-gomez"));
  CHECK_EQ(ascii_words("Łukasz Żółć"), std::string("lukasz zolc"));
  CHECK_EQ(ascii_words("Straße"), std::string("strasse"));
  CHECK_EQ(ascii_words("Ægir Þór"), std::string("aegir thor"));
  CHECK_EQ(ascii_words("Ștefan Țepeș"), std::string("stefan tepes"));
  CHECK_EQ(ascii_words("O'Brien"), std::string("obrien"));
  CHECK_EQ(ascii_words("--Jean-Luc--  Picard"), std::string("jean-luc picard"));
  CHECK_EQ(ascii_words("Иван Петров"), std::string(""));
  CHECK_EQ(ascii_words("Budi 2"), std::string("budi 2"));
  CHECK_EQ(ascii_words(std::string("bad\xff\xfe utf")), std::string("bad utf"));
}

static void test_user_names() {
  CHECK(!user_name_problem("").empty());
  CHECK(!user_name_problem("Zqhugo").empty());           // capitals
  CHECK(!user_name_problem("1zqhugo").empty());          // starts with a digit
  CHECK(!user_name_problem("_zqhugo").empty());
  CHECK(!user_name_problem("zq.hugo").empty());
  CHECK(!user_name_problem("zq hugo").empty());
  CHECK(!user_name_problem("zqhugo$").empty());
  CHECK(!user_name_problem(std::string(33, 'z')).empty());
  CHECK(user_name_problem(std::string(32, 'z')).empty());
  CHECK(!user_name_problem("root").empty());
  CHECK(!user_name_problem("sudo").empty());
  CHECK(!user_name_problem("nobody").empty());
  CHECK(user_name_problem("zqhugo").empty());
  CHECK(user_name_problem("zq_hu-go9").empty());
  CHECK(!user_name_problem("zqhugo", {"sam", "zqhugo"}).empty());
  CHECK(name_in_use("root"));
  CHECK(!name_in_use("zqhugo-not-a-user"));

  CHECK_EQ(suggest_user_name("Zqhugo Moniz do Rego"), std::string("zqhugo"));
  CHECK_EQ(suggest_user_name("Zqhugo Moniz do Rego", {"zqhugo"}), std::string("zqhugorego"));
  CHECK_EQ(suggest_user_name("Zqhugo Moniz do Rego", {"zqhugo", "zqhugorego"}), std::string("zrego"));
  CHECK_EQ(suggest_user_name("Zqhugo Moniz do Rego", {"zqhugo", "zqhugorego", "zrego"}),
           std::string("zqhugo2"));
  CHECK_EQ(suggest_user_name("Zqélodie"), std::string("zqelodie"));
  CHECK_EQ(suggest_user_name("2pac"), std::string("u2pac"));
  CHECK_EQ(suggest_user_name("Иван"), std::string(""));
  CHECK_EQ(suggest_user_name(""), std::string(""));
  // "root" is taken: the next spelling.
  CHECK_EQ(suggest_user_name("Root Zqbeer"), std::string("rootzqbeer"));
  // Long names stay within 32 characters.
  std::string longest = suggest_user_name(std::string(40, 'q') + " Zqlast");
  CHECK(longest.size() == 32 && user_name_problem(longest).empty());
}

static void test_passwords() {
  CHECK(password_strength("short1") == 0);
  CHECK(password_strength("aaaaaaaaaa") == 1);
  CHECK(password_strength("password") == 1);
  CHECK(password_strength("PassWord") == 1);
  CHECK(password_strength("12345678") == 1);
  CHECK(password_strength("abcdefgh") == 1);
  CHECK(password_strength("Tr0ub4dor&3") >= 2);
  CHECK(password_strength("correct horse battery staple") == 4);
  CHECK(password_strength("zqhugo!Sunny7", "zqhugo") < password_strength("rafter!Sunny7", "zqhugo"));
  CHECK(password_strength("Moniz2024!!", "", "Hugo Moniz") < password_strength("Bakso2024!!", "", "Hugo Moniz"));
  CHECK_EQ(strength_label(0), std::string("Too short"));
  CHECK_EQ(strength_label(4), std::string("Strong"));

  CHECK(!password_problem("", "").empty());
  CHECK(!password_problem("short1", "short1").empty());
  CHECK(!password_problem("zqhugo12", "zqhugo12", "zqhugo12").empty());
  CHECK(!password_problem("goodpass", "goodpasx").empty());
  CHECK(!password_problem("line\nbreak1", "line\nbreak1").empty());
  CHECK(!password_problem("tab\tbreak1", "tab\tbreak1").empty());
  CHECK(password_problem("validpass", "validpass").empty());
  // Eight characters, not eight bytes.
  CHECK(password_problem("ééééééé", "ééééééé").size() > 0);
  CHECK(password_problem("éééééééé", "éééééééé").empty());

  std::string a = hash_password("correct horse battery staple");
  std::string b = hash_password("correct horse battery staple");
  CHECK(starts_with(a, "$y$") || starts_with(a, "$6$"));
  CHECK(a != b);  // a fresh salt each time
  struct crypt_data data = {};
  const char *again = crypt_r("correct horse battery staple", a.c_str(), &data);
  CHECK(again && a == again);
  const char *wrong = crypt_r("correct horse battery stapler", a.c_str(), &data);
  CHECK(!wrong || a != wrong);
}

static User person(const std::string &name, Type type, bool self = false, bool locked = false) {
  User u;
  u.name = name;
  u.path = "/org/freedesktop/Accounts/User_" + name;
  u.type = type;
  u.self = self;
  u.locked = locked;
  u.uid = uid_t(1000 + name.size());
  return u;
}

static void test_rules() {
  User me = person("me", Type::standard, true), boss = person("boss", Type::administrator);
  User sam = person("sam", Type::standard);
  std::vector<User> all = {me, boss, sam};
  CHECK(active_admins(all) == 1);
  CHECK(!why_not_remove(all, me).empty());
  CHECK(!why_not_lock(all, me).empty());
  CHECK(!why_not_set_type(all, me, Type::administrator).empty());
  CHECK(why_not_set_type(all, me, Type::standard).empty());  // no change
  CHECK(!why_not_remove(all, boss).empty());                 // the last administrator
  CHECK(!why_not_lock(all, boss).empty());
  CHECK(!why_not_set_type(all, boss, Type::standard).empty());
  CHECK(why_not_remove(all, sam).empty());
  CHECK(why_not_lock(all, sam).empty());
  CHECK(why_not_set_type(all, sam, Type::administrator).empty());
  // A second administrator; a locked one does not count.
  User deputy = person("deputy", Type::administrator, false, true);
  all.push_back(deputy);
  CHECK(active_admins(all) == 1);
  CHECK(!why_not_remove(all, boss).empty());
  CHECK(why_not_remove(all, deputy).empty());
  all.back().locked = false;
  CHECK(active_admins(all) == 2);
  CHECK(why_not_remove(all, boss).empty());
  CHECK(why_not_set_type(all, boss, Type::standard).empty());
  CHECK(why_not_lock(all, boss).empty());
  CHECK_EQ(type_label(Type::administrator), std::string("Administrator"));
}

// Records the calls; |fail| names one that is refused.
class FakeBackend : public Backend {
 public:
  std::vector<std::string> calls;
  std::string fail;
  bool done(const std::string &call, std::string *error) {
    calls.push_back(call);
    if (!fail.empty() && starts_with(call, fail)) {
      if (error) *error = "refused by the fake";
      return false;
    }
    return true;
  }
  bool list(std::vector<User> *users, std::string *error) override {
    users->clear();
    return done("list", error);
  }
  bool create(const std::string &name, const std::string &real_name, Type type, std::string *path,
              std::string *error) override {
    if (path) *path = "/org/freedesktop/Accounts/User2000";
    return done("create " + name + " " + real_name + " " + std::to_string(int(type)), error);
  }
  bool remove(const User &u, bool remove_files, std::string *error) override {
    return done("remove " + u.name + (remove_files ? " files" : " keep"), error);
  }
  bool set_type(const User &u, Type type, std::string *error) override {
    return done("type " + u.name + " " + std::to_string(int(type)), error);
  }
  bool set_password(const User &u, const std::string &hash, std::string *error) override {
    return done("password " + u.path + " " + hash.substr(0, 3), error);
  }
  bool set_password_mode(const User &u, PasswordMode mode, std::string *error) override {
    return done("mode " + u.path + " " + std::to_string(int(mode)), error);
  }
  bool set_locked(const User &u, bool locked, std::string *error) override {
    return done("lock " + u.name + (locked ? " true" : " false"), error);
  }
  bool set_auto_login(const User &u, bool on, std::string *error) override {
    return done("autologin " + u.name + (on ? " true" : " false"), error);
  }
  bool set_real_name(const User &u, const std::string &name, std::string *error) override {
    return done("name " + u.name + " " + name, error);
  }
  bool set_icon(const User &u, const std::string &file, std::string *error) override {
    return done("icon " + u.name + " " + (file.empty() ? "none" : "file"), error);
  }
};

static void test_changes(const std::string &tmp) {
  FakeBackend fake;
  set_backend(&fake);
  std::vector<User> all = {person("me", Type::administrator, true), person("sam", Type::standard)};
  std::string error;
  NewUser n;
  n.name = "zqrobin";
  n.real_name = "  Robin Example ";
  n.password = n.again = "correct horse battery staple";
  CHECK(add_user(n, all, &error));
  CHECK(fake.calls.size() == 2);
  CHECK(fake.calls.size() == 2 && fake.calls[0] == "create zqrobin Robin Example 0");
  std::string hashed = fake.calls.size() == 2 ? fake.calls[1] : "";
  CHECK(hashed == "password /org/freedesktop/Accounts/User2000 $y$" ||
        hashed == "password /org/freedesktop/Accounts/User2000 $6$");
  // The user chooses a password at the first login; an administrator.
  fake.calls.clear();
  n.password_at_login = true;
  n.password = n.again = "";
  n.type = Type::administrator;
  CHECK(add_user(n, all, &error));
  CHECK(fake.calls.size() == 2 && fake.calls[0] == "create zqrobin Robin Example 1" &&
        fake.calls[1] == "mode /org/freedesktop/Accounts/User2000 1");
  // Refused before anything is asked: a taken name, no full name,
  // passwords that differ, a bad full name.
  fake.calls.clear();
  n.name = "sam";
  CHECK(!add_user(n, all, &error) && !error.empty());
  n.name = "zqrobin";
  n.real_name = " ";
  CHECK(!add_user(n, all, &error) && !error.empty());
  n.real_name = "Robin: Example";
  CHECK(!add_user(n, all, &error) && !error.empty());
  n.real_name = "Robin Example";
  n.password_at_login = false;
  n.password = "correct horse battery staple";
  n.again = "correct horse battery stapler";
  CHECK(!add_user(n, all, &error) && !error.empty());
  CHECK(fake.calls.empty());
  // AccountsService refuses (polkit): nothing more is asked.
  n.again = n.password;
  fake.fail = "create";
  CHECK(!add_user(n, all, &error) && error == "refused by the fake");
  CHECK(fake.calls.size() == 1);
  // The account was made, its password not.
  fake.calls.clear();
  fake.fail = "password";
  CHECK(!add_user(n, all, &error) && error.find("zqrobin") != std::string::npos &&
        error.find("refused by the fake") != std::string::npos);
  fake.fail.clear();

  // Another person's password.
  fake.calls.clear();
  User sam = all[1];
  CHECK(!set_password(sam, false, "short", "short", &error) && fake.calls.empty());
  CHECK(set_password(sam, false, "a long sentence", "a long sentence", &error));
  CHECK(set_password(sam, true, "", "", &error));
  CHECK(fake.calls.size() == 2 && starts_with(fake.calls[0], "password " + sam.path + " $") &&
        fake.calls[1] == "mode " + sam.path + " 1");

  // A picture for the login screen: a PNG of 256 x 256 is handed over.
  fake.calls.clear();
  std::string svg = tmp + "/face.svg";
  std::ofstream(svg) << "<svg xmlns='http://www.w3.org/2000/svg' width='40' height='30'>"
                        "<rect width='40' height='30' fill='#3a7'/></svg>";
  CHECK(set_picture(sam, svg, &error));
  CHECK(set_picture(sam, "", &error));
  CHECK(!set_picture(sam, tmp + "/missing.png", &error) && !error.empty());
  CHECK(fake.calls.size() == 2 && fake.calls[0] == "icon sam file" && fake.calls[1] == "icon sam none");
  set_backend(nullptr);
}

// A passwd(1) of the tests: the prompts of Debian's, the answers checked
// against $FAKE_OLD and $FAKE_NEW.
static std::string fake_passwd(const std::string &dir, const std::string &name, const std::string &body) {
  std::string path = dir + "/" + name;
  std::ofstream(path) << "#!/bin/sh\n"
                         "[ \"$LC_ALL\" = C ] || { echo 'passwd: not the C locale'; exit 3; }\n"
                         "stty -echo 2>/dev/null\n"
                      << body;
  chmod(path.c_str(), 0755);
  return path;
}

static void test_own_password(const std::string &tmp) {
  setenv("FAKE_OLD", "old secret 1", 1);
  setenv("FAKE_NEW", "new secret 22", 1);
  setenv("LC_ALL", "de_DE.UTF-8", 1);  // replaced by C for passwd
  const std::string ask =
      "printf 'Changing password for zq.\\nCurrent password: '; read cur; printf '\\n'\n"
      "[ \"$cur\" = \"$FAKE_OLD\" ] || { echo 'passwd: Authentication token manipulation error'; "
      "echo 'passwd: password unchanged'; exit 10; }\n"
      "printf 'New password: '; read new; printf '\\n'\n";
  std::string ok = fake_passwd(tmp, "passwd-ok", ask +
      "printf 'Retype new password: '; read again; printf '\\n'\n"
      "[ \"$new\" = \"$FAKE_NEW\" ] && [ \"$again\" = \"$FAKE_NEW\" ] || exit 4\n"
      "echo 'passwd: password updated successfully'\n");
  std::string rules = fake_passwd(tmp, "passwd-rules", ask +
      "echo 'BAD PASSWORD: The password is a palindrome'\n"
      "printf 'New password: '; read new; exit 5\n");
  std::string error;
  CHECK(change_own_password("old secret 1", "new secret 22", "new secret 22", &error, ok));
  CHECK(!change_own_password("wrong secret", "new secret 22", "new secret 22", &error, ok));
  CHECK_EQ(error, std::string("The current password is not correct."));
  CHECK(!change_own_password("old secret 1", "new secret 22", "new secret 22", &error, rules));
  CHECK(error.find("The password is a palindrome") != std::string::npos);
  CHECK(error.find("new secret") == std::string::npos);
  // Checked before passwd runs.
  CHECK(!change_own_password("old secret 1", "short", "short", &error, ok) && !error.empty());
  CHECK(!change_own_password("old secret 1", "new secret 22", "new secret 23", &error, ok));
  CHECK(!change_own_password("", "new secret 22", "new secret 22", &error, ok));
  CHECK(!change_own_password("new secret 22", "new secret 22", "new secret 22", &error, ok));
  CHECK(!change_own_password("old secret 1", "new secret 22", "new secret 22", &error, tmp + "/no-passwd"));
}

static void test_local_users() {
  std::vector<User> users = local_users();
  // Yours is always there, first.
  CHECK(!users.empty() && users[0].self && users[0].uid == getuid());
}

int main() {
  char tmpl[] = "/tmp/sde-accounts-test-XXXXXX";
  char *dir = mkdtemp(tmpl);
  if (!dir) return 2;
  std::string tmp = dir;
  setenv("XDG_RUNTIME_DIR", tmp.c_str(), 1);
  test_spelling();
  test_user_names();
  test_passwords();
  test_rules();
  test_changes(tmp);
  test_own_password(tmp);
  test_local_users();
  std::string cmd = "rm -rf '" + tmp + "'";
  if (system(cmd.c_str()) != 0) fprintf(stderr, "could not remove %s\n", tmp.c_str());
  if (g_failures) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("sde-accounts-tests: all passed\n");
  return 0;
}
