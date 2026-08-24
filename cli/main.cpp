#include "model_cache/cache.hpp"
#include "model_cache/detail/platform.hpp"
#include "model_cache/digest.hpp"
#include "model_cache/error.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace mc = model_cache;
using mc::ArtifactId;
using mc::ArtifactNamespace;
using mc::Config;
using mc::IngestSpec;
using mc::ModelCache;
using mc::ValidationMode;

namespace {

std::string json_escape(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out += c;
    }
  }
  out += "\"";
  return out;
}

struct Cli {
  bool json = false;
  std::string root = ".model-cache";
  ModelCache* cache = nullptr;

  void ensure_cache() {
    if (!cache) {
      Config cfg;
      cache = new ModelCache(root, cfg);
    }
  }

  int parse_key(const std::string& key, ArtifactId& out) {
    auto slash = key.find('/');
    if (slash == std::string::npos) {
      out = ArtifactId(ArtifactNamespace("default"), key);
    } else {
      out = ArtifactId(ArtifactNamespace(key.substr(0, slash)), key.substr(slash + 1));
    }
    return 0;
  }

  std::string file_content(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { std::fprintf(stderr, "error: cannot open %s\n", path.c_str()); return ""; }
    std::ostringstream ss; ss << in.rdbuf();
    return ss.str();
  }
};

void print_help() {
  std::printf(
      "model-cache 1.0.0 — reuse reusable model artifacts explicitly.\n"
      "usage: model-cache [--root <dir>] [--json] <command> [args]\n\n"
      "commands:\n"
      "  info                        show runtime/config info\n"
      "  ingest --key k --type T --role R --version V --file f\n"
      "  get --key k                 exact artifact lookup\n"
      "  lookup --type T --role R --arch A --precision P --model M --revision R\n"
      "  list [--type T]             list artifacts\n"
      "  variants --key k            list compatible variants\n"
      "  manifest-add --model M --revision R --artifacts a,b --roles r=a,b\n"
      "  manifest-get --model M --revision R\n"
      "  validate --key k            revalidate content\n"
      "  quarantine --key k [--reason r]\n"
      "  purge --key k\n"
      "  retire --key k\n"
      "  supersede --key k [--suffix s]\n"
      "  alias-set --alias a --key k\n"
      "  alias-get --alias a\n"
      "  pin --key k | unpin --key k\n"
      "  dependencies --key k | dependents --key k | ancestors --key k\n"
      "  stats | snapshot | explain --key k\n"
      "  save | restore | verify | gc\n"
      "  benchmark [--n N]           run a quick benchmark\n"
      "  --help                      this help\n");
}

}  // namespace

int main(int argc, char** argv) {
  Cli cli;
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

  // Parse global flags (--root, --json) and find the first command token.
  std::string cmd;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--json") cli.json = true;
    else if (args[i] == "--root") { if (i + 1 < args.size()) { cli.root = args[i + 1]; ++i; } }
    else if (cmd.empty()) cmd = args[i];
  }
  if (cmd.empty()) cmd = "help";

  auto need = [&](const std::string& flag, const std::string& dflt) -> std::string {
    for (std::size_t i = 1; i < args.size(); ++i)
      if (args[i] == flag && i + 1 < args.size()) return args[i + 1];
    return dflt;
  };

  if (cmd == "help" || cmd == "--help") { print_help(); return 0; }

  try {
    if (cmd == "info") {
      cli.ensure_cache();
      auto s = cli.cache->snapshot();
      if (cli.json) {
        std::printf("{\"version\":\"1.0.0\",\"root\":%s,\"logical\":%llu,\"physical\":%llu,\"physical_bytes\":%llu,\"logical_bytes\":%llu,\"dedup_saved\":%llu,\"capacity\":%llu}\n",
                    json_escape(cli.root).c_str(), (unsigned long long)s.logical_artifact_count,
                    (unsigned long long)s.physical_backing_count, (unsigned long long)s.total_physical_bytes,
                    (unsigned long long)s.total_logical_bytes, (unsigned long long)s.dedup_bytes_saved,
                    (unsigned long long)s.capacity_bytes);
      } else {
        std::printf("model-cache 1.0.0\nroot: %s\nlogical artifacts: %llu\nphysical backings: %llu\nphysical bytes: %llu\nlogical bytes: %llu\ndedup saved: %llu\ncapacity: %llu\n",
                    cli.root.c_str(), (unsigned long long)s.logical_artifact_count,
                    (unsigned long long)s.physical_backing_count, (unsigned long long)s.total_physical_bytes,
                    (unsigned long long)s.total_logical_bytes, (unsigned long long)s.dedup_bytes_saved,
                    (unsigned long long)s.capacity_bytes);
      }
      return 0;
    }

    if (cmd == "ingest") {
      cli.ensure_cache();
      ArtifactId id;
      cli.parse_key(need("--key", ""), id);
      const std::string path = need("--file", "");
      if (path.empty()) { std::fprintf(stderr, "ingest requires --file\n"); return 2; }
      IngestSpec s;
      const std::string t = need("--type", "MODEL_WEIGHTS");
      s.type = mc::parse_artifact_type(t);
      s.role = mc::ArtifactRole(need("--role", "weights"));
      s.version = mc::ArtifactVersion(need("--version", "1"));
      if (!need("--format", "").empty()) s.format = mc::ArtifactFormat(need("--format", ""));
      const auto content = cli.file_content(path);
      auto r = cli.cache->ingest(id, s, reinterpret_cast<const std::uint8_t*>(content.data()), content.size());
      if (cli.json) {
        std::printf("{\"admitted\":%s,\"key\":%s,\"digest\":%s,\"generation\":%llu,\"size\":%llu,\"dedup\":%s}\n",
                    r.admitted ? "true" : "false", json_escape(r.artifact_key).c_str(),
                    json_escape(r.digest.hex()).c_str(), (unsigned long long)r.generation,
                    (unsigned long long)r.size, r.dedup ? "true" : "false");
      } else {
        std::printf("admitted=%s key=%s digest=%s generation=%llu size=%llu dedup=%s\n",
                    r.admitted ? "true" : "false", r.artifact_key.c_str(), r.digest.hex().c_str(),
                    (unsigned long long)r.generation, (unsigned long long)r.size, r.dedup ? "true" : "false");
      }
      return r.admitted ? 0 : 1;
    }

    if (cmd == "get" || cmd == "lookup") {
      cli.ensure_cache();
      mc::LookupRequest req;
      const std::string key = need("--key", "");
      if (!key.empty()) { ArtifactId id; cli.parse_key(key, id); req.id = id; }
      if (!need("--type", "").empty()) req.type = mc::parse_artifact_type(need("--type", ""));
      if (!need("--role", "").empty()) req.role = mc::ArtifactRole(need("--role", ""));
      if (!need("--arch", "").empty()) req.architecture = mc::ArtifactArchitecture(need("--arch", ""));
      if (!need("--model", "").empty()) req.model = mc::ModelIdentity(need("--model", ""));
      if (!need("--revision", "").empty()) req.revision = mc::ModelRevision(need("--revision", ""));
      auto lr = cli.cache->lookup(req);
      if (cli.json) {
        std::printf("{\"found\":%s,\"key\":%s,\"validation\":%s,\"generation\":%llu,\"size\":%llu,\"digest\":%s}\n",
                    lr.found ? "true" : "false", json_escape(lr.artifact_key).c_str(),
                    mc::to_string(lr.validation), (unsigned long long)lr.generation,
                    (unsigned long long)lr.size, lr.content_digest ? json_escape(lr.content_digest->hex()).c_str() : "null");
      } else {
        std::printf("found=%d key=%s validation=%s generation=%llu size=%llu\n", lr.found ? 1 : 0,
                    lr.artifact_key.c_str(), mc::to_string(lr.validation),
                    (unsigned long long)lr.generation, (unsigned long long)lr.size);
        if (lr.content_digest) std::printf("digest=%s\n", lr.content_digest->hex().c_str());
      }
      return lr.found ? 0 : 1;
    }

    if (cmd == "list") {
      cli.ensure_cache();
      std::optional<mc::ArtifactType> ty;
      if (!need("--type", "").empty()) ty = mc::parse_artifact_type(need("--type", ""));
      auto keys = cli.cache->list(ty);
      if (cli.json) { std::printf("{\"artifacts\":["); for (std::size_t i=0;i<keys.size();++i){ if(i)std::printf(","); std::printf("%s", json_escape(keys[i]).c_str()); } std::printf("]}\n"); }
      else for (auto& k : keys) std::printf("%s\n", k.c_str());
      return 0;
    }

    if (cmd == "variants") {
      cli.ensure_cache(); ArtifactId id; cli.parse_key(need("--key",""), id);
      auto v = cli.cache->variants(id);
      if (cli.json) { std::printf("{\"variants\":["); for (std::size_t i=0;i<v.size();++i){ if(i)std::printf(","); std::printf("%s", json_escape(v[i]).c_str()); } std::printf("]}\n"); }
      else for (auto& k : v) std::printf("%s\n", k.c_str());
      return 0;
    }

    if (cmd == "manifest-add") {
      cli.ensure_cache();
      mc::Manifest m;
      m.model = mc::ModelIdentity(need("--model", ""));
      m.revision = mc::ModelRevision(need("--revision", ""));
      const std::string arts = need("--artifacts", "");
      std::stringstream ss(arts); std::string tok;
      while (std::getline(ss, tok, ',')) if (!tok.empty()) m.artifacts.push_back(tok);
      cli.cache->manifest_add(m);
      std::printf("manifest added: %s\n", m.canonical_key().c_str());
      return 0;
    }

    if (cmd == "manifest-get") {
      cli.ensure_cache();
      auto m = cli.cache->manifest_get(mc::ModelIdentity(need("--model","")), mc::ModelRevision(need("--revision","")));
      if (!m) { std::fprintf(stderr, "manifest not found\n"); return 1; }
      if (cli.json) { std::printf("{\"model\":%s,\"revision\":%s,\"validation\":%s,\"artifacts\":[", json_escape(m->model.str()).c_str(), json_escape(m->revision.str()).c_str(), mc::to_string(m->validation)); for(std::size_t i=0;i<m->artifacts.size();++i){if(i)std::printf(","); std::printf("%s", json_escape(m->artifacts[i]).c_str());} std::printf("]}\n"); }
      else { std::printf("model=%s revision=%s validation=%s\n", m->model.str().c_str(), m->revision.str().c_str(), mc::to_string(m->validation)); for (auto& a : m->artifacts) std::printf("  %s\n", a.c_str()); }
      return 0;
    }

    if (cmd == "validate" || cmd == "quarantine" || cmd == "purge" || cmd == "retire" || cmd == "supersede") {
      cli.ensure_cache(); ArtifactId id; cli.parse_key(need("--key",""), id);
      if (cmd == "validate") cli.cache->validate(id);
      else if (cmd == "quarantine") cli.cache->quarantine(id, need("--reason", "cli"));
      else if (cmd == "purge") cli.cache->purge(id);
      else if (cmd == "retire") cli.cache->retire(id);
      else cli.cache->supersede(id, need("--suffix", ""));
      std::printf("%s ok: %s\n", cmd.c_str(), id.canonical().c_str());
      return 0;
    }

    if (cmd == "alias-set" || cmd == "alias-get") {
      cli.ensure_cache();
      const std::string alias = need("--alias", "");
      if (cmd == "alias-set") { ArtifactId id; cli.parse_key(need("--key",""), id); cli.cache->alias_set(alias, id); }
      else { auto v = cli.cache->alias_get(alias); if (!v) { std::fprintf(stderr, "alias not found\n"); return 1; } std::printf("%s\n", v->c_str()); }
      return 0;
    }

    if (cmd == "pin" || cmd == "unpin") {
      cli.ensure_cache(); ArtifactId id; cli.parse_key(need("--key",""), id);
      if (cmd == "pin") cli.cache->pin(id); else cli.cache->unpin(id);
      std::printf("%s ok: %s\n", cmd.c_str(), id.canonical().c_str());
      return 0;
    }

    if (cmd == "dependencies" || cmd == "dependents" || cmd == "ancestors") {
      cli.ensure_cache(); ArtifactId id; cli.parse_key(need("--key",""), id);
      std::vector<std::string> out = (cmd=="dependencies") ? cli.cache->dependencies(id) : (cmd=="dependents") ? cli.cache->dependents(id) : cli.cache->ancestors(id);
      if (cli.json) { std::printf("{\"nodes\":["); for(std::size_t i=0;i<out.size();++i){if(i)std::printf(","); std::printf("%s", json_escape(out[i]).c_str());} std::printf("]}\n"); }
      else for (auto& n : out) std::printf("%s\n", n.c_str());
      return 0;
    }

    if (cmd == "explain") {
      cli.ensure_cache();
      mc::LookupRequest req;
      if (!need("--key", "").empty()) { ArtifactId id; cli.parse_key(need("--key",""), id); req.id = id; }
      if (!need("--role", "").empty()) req.role = mc::ArtifactRole(need("--role", ""));
      if (!need("--type", "").empty()) req.type = mc::parse_artifact_type(need("--type", ""));
      auto e = cli.cache->explain(req);
      if (cli.json) {
        std::printf("{\"requested\":%s,\"selected\":%s,\"validation\":%s,\"generation\":%llu,\"action\":%s,\"candidates\":%zu,\"rejected\":%zu}\n",
                    json_escape(e.requested_identity).c_str(), e.selected_artifact ? json_escape(*e.selected_artifact).c_str() : "null",
                    mc::to_string(e.validation), (unsigned long long)e.generation, mc::to_string(e.selected_action),
                    e.candidates_evaluated.size(), e.candidates_rejected.size());
      } else {
        std::printf("requested=%s\nselected_action=%s\nvalidation=%s\ngeneration=%llu\ncandidates=%zu\n",
                    e.requested_identity.c_str(), mc::to_string(e.selected_action), mc::to_string(e.validation),
                    (unsigned long long)e.generation, e.candidates_evaluated.size());
        if (e.selected_artifact) std::printf("selected=%s\n", e.selected_artifact->c_str());
        for (auto& cr : e.candidates_rejected) std::printf("  rejected: %s (%s)\n", cr.first.c_str(), cr.second.c_str());
      }
      return 0;
    }

    if (cmd == "snapshot" || cmd == "stats") {
      cli.ensure_cache();
      auto s = cli.cache->snapshot();
      if (cli.json) {
        std::printf("{\"logical\":%llu,\"physical\":%llu,\"logical_bytes\":%llu,\"physical_bytes\":%llu,\"dedup_saved\":%llu,\"valid\":%llu,\"quarantined\":%llu,\"manifests\":%llu,\"aliases\":%llu,\"edges\":%llu,\"hits\":%llu,\"misses\":%llu,\"evicted\":%llu}\n",
                    (unsigned long long)s.logical_artifact_count, (unsigned long long)s.physical_backing_count,
                    (unsigned long long)s.total_logical_bytes, (unsigned long long)s.total_physical_bytes,
                    (unsigned long long)s.dedup_bytes_saved, (unsigned long long)s.valid_count,
                    (unsigned long long)s.quarantined_count, (unsigned long long)s.manifest_count,
                    (unsigned long long)s.alias_count, (unsigned long long)s.dependency_edge_count,
                    (unsigned long long)s.hits, (unsigned long long)s.misses, (unsigned long long)s.evicted);
      } else {
        std::printf("logical=%llu physical=%llu logical_bytes=%llu physical_bytes=%llu dedup_saved=%llu valid=%llu quarantined=%llu manifests=%llu aliases=%llu edges=%llu hits=%llu misses=%llu evicted=%llu\n",
                    (unsigned long long)s.logical_artifact_count, (unsigned long long)s.physical_backing_count,
                    (unsigned long long)s.total_logical_bytes, (unsigned long long)s.total_physical_bytes,
                    (unsigned long long)s.dedup_bytes_saved, (unsigned long long)s.valid_count,
                    (unsigned long long)s.quarantined_count, (unsigned long long)s.manifest_count,
                    (unsigned long long)s.alias_count, (unsigned long long)s.dependency_edge_count,
                    (unsigned long long)s.hits, (unsigned long long)s.misses, (unsigned long long)s.evicted);
      }
      return 0;
    }

    if (cmd == "verify") { cli.ensure_cache(); std::printf("corrupt objects: %zu\n", cli.cache->verify()); return 0; }
    if (cmd == "gc") { cli.ensure_cache(); std::printf("gc removed: %zu\n", cli.cache->gc()); return 0; }
    if (cmd == "save") { cli.ensure_cache(); cli.cache->save(); std::printf("saved\n"); return 0; }
    if (cmd == "restore") { cli.ensure_cache(); cli.cache->restore(); std::printf("restored\n"); return 0; }

    if (cmd == "benchmark") {
      cli.ensure_cache();
      const int n = std::atoi(need("--n", "1000").c_str());
      const std::string payload(4096, 'x');
      const auto t0 = std::chrono::steady_clock::now();
      for (int i = 0; i < n; ++i) {
        ArtifactId id(ArtifactNamespace("bench"), "a" + std::to_string(i % 100));
        IngestSpec s; s.type = mc::ArtifactType::MODEL_WEIGHTS; s.role = mc::ArtifactRole("w"); s.version = mc::ArtifactVersion("1");
        cli.cache->ingest(id, s, reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
      }
      const auto t1 = std::chrono::steady_clock::now();
      const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
      std::printf("ingested %d (dedup=%d distinct) in %.2f ms (%.1f us/op)\n", n, (n % 100) + 1, ms, ms * 1000.0 / n);
      return 0;
    }

    std::fprintf(stderr, "unknown command: %s (run model-cache --help)\n", cmd.c_str());
    return 2;
  } catch (const mc::Error& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
