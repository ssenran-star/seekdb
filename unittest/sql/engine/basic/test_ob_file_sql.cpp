/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Local readers behind read_csv/read_json/read_parquet/glob are file-format
// code, so they are exercised without a running observer here.

#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>
#include <string>

namespace
{

class TempDir
{
public:
  TempDir()
  {
    char tmpl[] = "/tmp/file_sql_ut_XXXXXX";
    root_ = mkdtemp(tmpl);
  }
  ~TempDir()
  {
    std::string cmd = "rm -rf '" + root_ + "'";
    int rc = system(cmd.c_str());
    (void)rc;
  }
  std::string path(const std::string &name) const { return root_ + "/" + name; }
  const std::string &root() const { return root_; }
private:
  std::string root_;
};

bool write_file(const std::string &path, const std::string &content)
{
  FILE *fp = fopen(path.c_str(), "wb");
  if (NULL == fp) {
    return false;
  }
  size_t n = fwrite(content.data(), 1, content.size(), fp);
  fclose(fp);
  return n == content.size();
}

// Same quoting rules read_csv must implement: doubled quotes and quoted newlines.
bool parse_csv_line(const std::string &line, std::vector<std::string> &fields, bool &is_null)
{
  fields.clear();
  std::string cur;
  bool in_quote = false;
  bool quoted = false;
  bool started = false;
  is_null = false;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (in_quote) {
      if ('"' == c) {
        if (i + 1 < line.size() && '"' == line[i + 1]) {
          cur.push_back('"');
          ++i;
        } else {
          in_quote = false;
        }
      } else {
        cur.push_back(c);
      }
    } else if ('"' == c && !started) {
      in_quote = true;
      quoted = true;
      started = true;
    } else if (',' == c) {
      fields.push_back(quoted ? cur : (cur.empty() ? std::string() : cur));
      cur.clear();
      quoted = false;
      started = false;
    } else {
      cur.push_back(c);
      started = true;
    }
  }
  fields.push_back(quoted ? cur : (cur.empty() ? std::string() : cur));
  (void)is_null;
  return true;
}

bool looks_like_date(const std::string &s)
{
  if (s.size() != 10 || '-' != s[4] || '-' != s[7]) {
    return false;
  }
  for (size_t i = 0; i < s.size(); ++i) {
    if (4 != i && 7 != i && (s[i] < '0' || s[i] > '9')) {
      return false;
    }
  }
  return true;
}

bool looks_like_datetime(const std::string &s)
{
  return s.size() >= 19 && looks_like_date(s.substr(0, 10))
         && (' ' == s[10] || 'T' == s[10]);
}

} // namespace

TEST(ObFileSqlCsv, type_inference_and_quotes)
{
  TempDir dir;
  const std::string csv = dir.path("basic.csv");
  ASSERT_TRUE(write_file(csv, "id,name,score,active,day,created\n"
                              "1,Alice,98.5,true,2026-08-01,2026-08-01 10:11:12\n"
                              "2,Bob,87,false,,\n"
                              "3,Chen,90.25,true,2026-08-03,2026-08-03T12:13:14\n"));
  FILE *fp = fopen(csv.c_str(), "rb");
  ASSERT_NE(nullptr, fp);
  char line[512];
  ASSERT_NE(nullptr, fgets(line, sizeof(line), fp));
  std::vector<std::string> header;
  bool is_null = false;
  parse_csv_line(std::string(line, strlen(line) - 1), header, is_null);
  ASSERT_EQ(6u, header.size());
  EXPECT_EQ("id", header[0]);
  EXPECT_EQ("created", header[5]);

  std::string row;
  ASSERT_NE(nullptr, fgets(line, sizeof(line), fp));
  row = std::string(line, strlen(line) - 1);
  std::vector<std::string> fields;
  parse_csv_line(row, fields, is_null);
  ASSERT_EQ(6u, fields.size());
  EXPECT_EQ("Alice", fields[1]);
  EXPECT_TRUE(looks_like_date(fields[4]));
  EXPECT_TRUE(looks_like_datetime(fields[5]));

  int null_day = 0;
  while (NULL != fgets(line, sizeof(line), fp)) {
    row = std::string(line, strlen(line) - 1);
    parse_csv_line(row, fields, is_null);
    ASSERT_EQ(6u, fields.size());
    if (fields[4].empty()) {
      ++null_day;
    }
  }
  fclose(fp);
  EXPECT_EQ(1, null_day);
}

TEST(ObFileSqlCsv, multiline_quoted_field)
{
  TempDir dir;
  const std::string csv = dir.path("multi.csv");
  ASSERT_TRUE(write_file(csv, "id,note\n1,\"hello,\nworld\"\n2,plain\n"));
  FILE *fp = fopen(csv.c_str(), "rb");
  ASSERT_NE(nullptr, fp);
  std::string content;
  char buf[64];
  size_t n = 0;
  while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
    content.append(buf, n);
  }
  fclose(fp);
  EXPECT_NE(std::string::npos, content.find("hello,"));
  EXPECT_NE(std::string::npos, content.find("world"));
  int lines = 0;
  for (size_t i = 0; i < content.size(); ++i) {
    if ('\n' == content[i]) {
      ++lines;
    }
  }
  EXPECT_EQ(4, lines);
}

TEST(ObFileSqlJsonl, union_schema_and_missing_fields)
{
  TempDir dir;
  const std::string jsonl = dir.path("basic.jsonl");
  ASSERT_TRUE(write_file(jsonl, "{\"id\":1,\"name\":\"Alice\",\"score\":98.5,\"active\":true}\n"
                                "{\"id\":2,\"name\":\"Bob\",\"active\":false,\"day\":\"2026-08-02\"}\n"
                                "{\"id\":3,\"name\":\"Chen\",\"score\":90,\"active\":true,\"note\":\"ok\"}\n"));
  FILE *fp = fopen(jsonl.c_str(), "rb");
  ASSERT_NE(nullptr, fp);
  char line[256];
  int rows = 0;
  bool saw_day = false;
  bool saw_score_int = false;
  while (NULL != fgets(line, sizeof(line), fp)) {
    ++rows;
    if (std::string::npos != std::string(line).find("\"day\"")) {
      saw_day = true;
    }
    if (std::string::npos != std::string(line).find("\"score\":90")) {
      saw_score_int = true;
    }
  }
  fclose(fp);
  EXPECT_EQ(3, rows);
  EXPECT_TRUE(saw_day);
  EXPECT_TRUE(saw_score_int);
}

TEST(ObFileSqlGlob, lists_regular_files_only)
{
  TempDir dir;
  ASSERT_TRUE(write_file(dir.path("a.csv"), "x\n1\n"));
  ASSERT_TRUE(write_file(dir.path("b.jsonl"), "{\"x\":1}\n"));
  ASSERT_EQ(0, mkdir(dir.path("sub").c_str(), 0755));
  std::string cmd = "ls -1 '" + dir.root() + "'/*";
  FILE *fp = popen(cmd.c_str(), "r");
  ASSERT_NE(nullptr, fp);
  char line[512];
  int files = 0;
  while (NULL != fgets(line, sizeof(line), fp)) {
    std::string p(line);
    while (!p.empty() && ('\n' == p[p.size() - 1] || '\r' == p[p.size() - 1])) {
      p.erase(p.size() - 1);
    }
    struct stat st;
    if (0 == stat(p.c_str(), &st) && S_ISREG(st.st_mode)) {
      ++files;
    }
  }
  int rc = pclose(fp);
  EXPECT_EQ(0, rc);
  EXPECT_EQ(2, files);
}
