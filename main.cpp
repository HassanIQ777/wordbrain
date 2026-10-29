#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using Words = std::vector<std::string>;

// A brain is nothing but counts: context -> (next word -> times seen).
// A context is 1..order words joined by spaces, e.g. "red apple is".
// The brain stores EVERY context length up to `order`, so the generator can
// fall back to a shorter context when the long one was never seen.
// Counts add up, so brains merge by plain addition.
struct Brain {
  std::map<std::string, std::map<std::string, long long>> table;
  int order = 1;
};

const std::string MAGIC_V1 =
    "WORDBRAIN 1"; // old files: one-word contexts only (still readable!)
const std::string MAGIC_V2 =
    "WORDBRAIN 2"; // current files: contexts of any length

// Any non-letter is a separator. Lowercases everything.
Words read_words(const std::string &path) {
  std::ifstream f(path);
  if (!f)
    throw std::runtime_error("can't open " + path);
  Words out;
  std::string w;
  char c;
  while (f.get(c)) {
    if (std::isalpha(static_cast<unsigned char>(c)))
      w += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    else if (!w.empty()) {
      out.push_back(w);
      w.clear();
    }
  }
  if (!w.empty())
    out.push_back(w);
  return out;
}

// Teach a brain one book: for every word, remember what preceded it
// (1 word back, 2 words back, ... up to `order`).
void learn(Brain &brain, const Words &text, int order) {
  brain.order = std::max(brain.order, order);
  for (size_t i = 1; i < text.size(); ++i) {
    std::string ctx;
    for (size_t k = 1; k <= static_cast<size_t>(order) && k <= i; ++k) {
      ctx = (k == 1) ? text[i - 1]
                     : text[i - k] + ' ' +
                           ctx; // grow the context one word to the left
      ++brain.table[ctx][text[i]];
    }
  }
}

// Loading ADDS to whatever is already in the brain, so loading a second file IS
// a merge. A line is: <context words...> <next word> <count>
void load_brain(Brain &brain, const std::string &path) {
  std::ifstream f(path);
  if (!f)
    throw std::runtime_error("can't open " + path);
  std::string line;
  std::getline(f, line);
  if (line != MAGIC_V1 && line != MAGIC_V2)
    throw std::runtime_error(path + " is not a brain file");

  for (long long lineno = 2; std::getline(f, line); ++lineno) {
    std::istringstream in(line);
    Words tok;
    for (std::string t; in >> t;)
      tok.push_back(t);
    if (tok.empty())
      continue;

    auto bad = [&] {
      return std::runtime_error(path + " is corrupted (line " +
                                std::to_string(lineno) + ")");
    };
    if (tok.size() < 3)
      throw bad();

    long long n = 0;
    size_t used = 0;
    try {
      n = std::stoll(tok.back(), &used);
    } catch (const std::exception &) {
      throw bad();
    }
    if (used != tok.back().size() || n <= 0)
      throw bad();

    std::string ctx = tok[0];
    for (size_t i = 1; i + 2 < tok.size(); ++i)
      ctx += ' ' + tok[i];
    brain.table[ctx][tok[tok.size() - 2]] += n;
    brain.order = std::max(brain.order, static_cast<int>(tok.size()) - 2);
  }
}

// std::map keeps keys sorted, so the same brain always produces the same file
// (diff-friendly).
void save_brain(const Brain &brain, const std::string &path) {
  std::ofstream f(path);
  if (!f)
    throw std::runtime_error("can't write " + path);
  f << MAGIC_V2 << '\n';
  for (const auto &[ctx, nexts] : brain.table)
    for (const auto &[word, n] : nexts)
      f << ctx << ' ' << word << ' ' << n << '\n';
  if (!f)
    throw std::runtime_error("failed while writing " + path);
}

size_t pair_count(const Brain &brain) {
  size_t total = 0;
  for (const auto &kv : brain.table)
    total += kv.second.size();
  return total;
}

int parse_int(const char *s, const std::string &what, int min_value) {
  try {
    size_t used = 0;
    int v = std::stoi(s, &used);
    if (used == std::string(s).size() && v >= min_value)
      return v;
  } catch (const std::exception &) {
  }
  throw std::runtime_error(what + " must be a whole number >= " +
                           std::to_string(min_value) + ", got '" + s + "'");
}

int run(int argc, char **argv) {
  const std::string VERSION = "26.10.30";
  const std::string mode = argc > 1 ? argv[1] : "";

  if (mode == "ver" && argc == 2) {
    std::cout << "Wordbrain version " << VERSION << "\n";
    return 0;
  }
  if (mode == "train" &&
      argc >= 5) { // train order out.brain book1.txt [book2.txt ...]
    const int order = parse_int(argv[2], "order", 1);
    Brain brain;
    for (int i = 4; i < argc; ++i)
      learn(brain, read_words(argv[i]), order);
    save_brain(brain, argv[3]);
    std::cout << "saved " << argv[3] << " (order " << brain.order << ", "
              << pair_count(brain) << " entries)\n";
    return 0;
  }

  if (mode == "merge" && argc >= 4) { // merge out.brain a.brain [b.brain ...]
    Brain brain;
    for (int i = 3; i < argc; ++i)
      load_brain(brain, argv[i]);
    save_brain(brain, argv[2]);
    std::cout << "saved " << argv[2] << " (order " << brain.order << ", "
              << pair_count(brain) << " entries)\n";
    return 0;
  }

  if (mode == "gen" && argc >= 5) { // gen count order a.brain [b.brain ...]
    const int count = parse_int(argv[2], "count", 0);
    const int cap = parse_int(argv[3], "order", 1);
    Brain brain;
    for (int i = 4; i < argc; ++i)
      load_brain(brain, argv[i]); // several brains get merged on the fly
    if (brain.table.empty())
      throw std::runtime_error("empty brain");
    const size_t order = static_cast<size_t>(
        std::min(cap, brain.order)); // can't look further back than it learned

    // Plain word popularity (from the 1-word contexts): used to start, and when
    // a dead end leaves nothing else.
    std::map<std::string, long long> freq;
    for (const auto &[ctx, nexts] : brain.table)
      if (ctx.find(' ') == std::string::npos)
        for (const auto &[w, n] : nexts)
          freq[w] += n;
    Words pop_words;
    std::vector<double> pop_weights;
    for (const auto &[w, n] : freq) {
      pop_words.push_back(w);
      pop_weights.push_back(static_cast<double>(n));
    }
    std::discrete_distribution<size_t> popular(pop_weights.begin(),
                                               pop_weights.end());

    std::mt19937 rng(std::random_device{}());

    // Look at the last `order` words; if that context was never seen, back off
    // to fewer words.
    auto pick = [&](const Words &so_far) -> std::string {
      for (size_t k = std::min(order, so_far.size()); k >= 1; --k) {
        std::string ctx = so_far[so_far.size() - k];
        for (size_t j = so_far.size() - k + 1; j < so_far.size(); ++j)
          ctx += ' ' + so_far[j];

        auto it = brain.table.find(ctx);
        if (it == brain.table.end())
          continue; // never seen this context -> try a shorter one

        Words cands;
        std::vector<double> weights;
        for (const auto &[w, n] : it->second) {
          cands.push_back(w);
          weights.push_back(static_cast<double>(n));
        }
        std::discrete_distribution<size_t> dice(weights.begin(), weights.end());
        return cands[dice(rng)];
      }
      return pop_words[popular(
          rng)]; // nothing matched at all (or we're just starting)
    };

    Words out;
    while (static_cast<int>(out.size()) < count)
      out.push_back(pick(out));
    for (const auto &w : out)
      std::cout << w << ' ';
    std::cout << '\n';
    return 0;
  }

  std::cerr << "Wordbrain " << VERSION << "\n"
            << "usage:\n"
            << "  " << argv[0]
            << " train order out.brain book1.txt [book2.txt ...]\n"
            << "  " << argv[0] << " merge out.brain a.brain [b.brain ...]\n"
            << "  " << argv[0] << " gen   count order a.brain [b.brain ...]\n"
            << "  " << argv[0] << " ver   prints program version\n"
            << "(order = how many previous words to look at; gen can use less "
               "than the brain learned)\n";
  return 1;
}

int main(int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << '\n';
    return 1;
  }
}
