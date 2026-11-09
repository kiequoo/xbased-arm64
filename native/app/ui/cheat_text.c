/*
 * xbased — the cheats' names (cheat_text.h).
 *
 * The Xbox scene's trainers name their options as their authors typed
 * them: "Inf Health", "infinite health", "INF HEALTH", "Infininte Health",
 * "Inf Health P1", "1 Hit Kill (Rt+D)", "Lotsa $$$". A label is read as
 * words: the abbreviations and misspellings spelled out, the player and the
 * buttons taken out and said after it, sentence case (the words the
 * trainers share lower case, a game's own names as they were). Then, for
 * the interface's language, the common names come from the tables below:
 * whole phrases, or "infinite" / "max" with a thing, its gender and the
 * language's word order. Anything else stays in English, cleaned.
 */
#include "native/app/ui/cheat_text.h"
#include "native/app/ui/i18n.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NT (L_COUNT - 1)    /* the translated languages, L_ES .. L_KO */

/* ------------------------------------------------------------ words */

/* The trainers' words that are English ones (a dictionary's, lower case
 * there) and the ones several games' trainers share: lower case inside a
 * name. Any other word is kept as the trainer wrote it (a game's own name:
 * Nimpo, Elvis Presley, SpeedBreaker). Sorted (strcmp). */
static const char *const k_common[] = {
    "abilities", "ability", "accept", "accidental", "ace", "acids", "acorn", "action", "activate",
    "add", "adjust", "adrenalin", "adrenaline", "advent", "after", "against", "age", "ai", "aid",
    "aim", "air", "alarm", "alert", "alignment", "all", "allow", "alt", "always", "alwys", "am",
    "ammo", "amplifier", "and", "antidotes", "antiques", "any", "anything", "apples", "arcade",
    "arctic", "are", "arena", "arenas", "armlet", "armor", "arms", "army", "around", "arrest",
    "arrow", "arrowheads", "arrows", "art", "as", "aspiration", "ass", "assist", "at", "atomic",
    "attach", "attack", "attacks", "attempts", "attribute", "attributes", "augment", "aura", "auto",
    "available", "away", "axes", "babe", "back", "backup", "bad", "badge", "balance", "bald",
    "ball", "balls", "banana", "bananas", "bandage", "bandages", "bar", "barbells", "bars", "bat",
    "batteries", "battle", "bazooka", "bean", "beans", "beating", "bet", "better", "big", "bigger",
    "bikers", "billion", "binocular", "bird", "bits", "black", "blacksmith", "bladder", "blaze",
    "blind", "blitzing", "block", "blocking", "blood", "blow", "blue", "board", "bob", "bolas",
    "bolts", "bomb", "bombs", "bonus", "boob", "book", "boost", "boosting", "boss", "bosses",
    "both", "bottles", "bounds", "bounty", "bow", "box", "brain", "break", "breaker", "breakers",
    "breath", "brew", "bribe", "bribes", "bridge", "bridges", "bright", "buckets", "build",
    "bullet", "bullets", "button", "buy", "buying", "camera", "can", "can't", "cannon", "cans",
    "cant", "canteen", "cap", "caps", "capsule", "capture", "car", "card", "cards", "career",
    "carnage", "cars", "cart", "cash", "censor", "censoring", "central", "chaff", "chain",
    "challenge", "challenges", "change", "changes", "character", "charge", "chars", "chase",
    "cheat", "cheats", "check", "checkpoints", "checks", "chests", "chi", "chimp", "chin", "chips",
    "chocolate", "choose", "chrome", "cht", "circus", "clash", "classes", "classic", "clean",
    "clear", "clip", "clips", "cloak", "cloaked", "clock", "clothes", "clubs", "coin", "coins",
    "cold", "combat", "combo", "comfort", "coming", "command", "commando", "complete", "composure",
    "con", "concentration", "concept", "connect", "continue", "continues", "controls", "cop",
    "cops", "core", "cosmetic", "cosmic", "cost", "costume", "count", "countdown", "counter",
    "courses", "cowboy", "cpu", "crack", "crappy", "crash", "crashes", "crawl", "crazy", "creation",
    "creature", "credits", "crew", "crib", "cribs", "critical", "crocodile", "crow", "crunch",
    "crystals", "curfew", "curious", "current", "cut", "cuts", "cycle", "damage", "dance", "dark",
    "darts", "data", "days", "dead", "deaf", "death", "debug", "decapitate", "decay", "decks",
    "decoy", "decoys", "decrease", "deduct", "deductions", "deliver", "demolition", "demon",
    "destruction", "dev", "develop", "developer", "developers", "devil", "devils", "diamonds",
    "die", "difficulty", "dig", "dirty", "disable", "discs", "disks", "distractions", "dives",
    "does", "dog", "dogs", "don", "don't", "dont", "doors", "dot", "double", "down", "downs",
    "dpad", "drain", "draining", "dressings", "drift", "drill", "drills", "drinks", "drive",
    "drowning", "drug", "drugs", "drunk", "dynamite", "each", "earn", "easy", "edited", "effect",
    "em", "empty", "enable", "encyclopedias", "end", "endings", "endurance", "enemies", "enemy",
    "energy", "engine", "enough", "equipment", "escape", "etc", "everything", "evidence", "evil",
    "exact", "exchange", "exercise", "exes", "exp", "experience", "extra", "extras", "extreme",
    "factions", "fail", "fails", "failure", "fairy", "fall", "falling", "falls", "fast", "faster",
    "fatal", "fatality", "fear", "feats", "feel", "felony", "feral", "festival", "field", "fights",
    "file", "files", "fill", "film", "films", "finish", "finishers", "fire", "first", "flares",
    "flash", "flashlight", "flask", "flight", "fly", "flying", "focus", "foes", "fog", "follow",
    "food", "footsteps", "for", "force", "forth", "fouls", "found", "fox", "frag", "fraud", "freak",
    "free", "freeze", "friendly", "friendship", "from", "frozen", "fruit", "fuel", "full", "fully",
    "fumbles", "fun", "gadgets", "gain", "gal", "gallery", "gambling", "game", "gamebreaker",
    "gameclock", "gameqtr", "games", "gang", "gauge", "gear", "gems", "gen", "get", "ghost",
    "ghosts", "girl", "give", "gliders", "glyphs", "gnome", "goal", "goals", "god", "gold",
    "golfers", "good", "googles", "gown", "grade", "graffiti", "gravity", "green", "grenade",
    "grenades", "grip", "ground", "guideline", "gun", "gunner", "guns", "guy", "hack", "hacked",
    "hacking", "hacks", "hard", "has", "have", "head", "heads", "healing", "health", "healthy",
    "heaps", "heart", "hearts", "heat", "heating", "hell", "her", "herb", "herbs", "hero", "hidden",
    "high", "highlight", "hit", "hits", "hlth", "hold", "hole", "holes", "home", "hostages",
    "house", "hp", "hr", "hud", "huge", "hull", "humans", "hunger", "hurt", "hygiene", "hyper",
    "id", "idle", "idols", "igk", "igks", "ignore", "ii", "import", "impound", "in", "inc",
    "increase", "incredible", "ind", "indicator", "inf", "infinate", "infininte", "infinita",
    "infinitas", "infinite", "infinitos", "ingredients", "insane", "instant", "int", "intel",
    "intercept", "interceptions", "intimidation", "intuition", "inventory", "invincibility",
    "invincible", "invisibility", "invisible", "invulnerability", "invulnerable", "ion", "iron",
    "is", "item", "items", "jade", "jail", "jars", "jays", "jet", "jewels", "juice", "jump", "just",
    "karma", "keep", "keeps", "key", "keys", "kill", "killer", "kills", "kit", "kits", "knockouts",
    "kudos", "land", "landing", "lap", "large", "laser", "last", "launcher", "lazy", "leaning",
    "least", "left", "legends", "level", "levels", "library", "life", "lifelines", "light", "limbo",
    "limit", "linemen", "lines", "lives", "load", "loader", "loads", "loadsa", "loans", "lock",
    "locks", "loco", "logos", "long", "longer", "looting", "lose", "loses", "loss", "lots", "lotsa",
    "loud", "lt", "lung", "lvl", "madden", "magic", "main", "make", "mana", "manual", "manufacture",
    "many", "maps", "mariachi", "married", "mass", "master", "mat", "match", "material", "matrix",
    "max", "maximum", "maze", "me", "med", "medal", "medals", "medical", "medikit", "menu", "meter",
    "mg", "midway", "might", "mil", "miles", "mill", "million", "millionaire", "mills", "mind",
    "minimum", "mirror", "miss", "missile", "missiles", "mission", "missions", "mo", "mod", "mode",
    "model", "modified", "mom", "money", "monkeys", "monster", "moon", "morale", "more", "morphine",
    "most", "motion", "motives", "motorcycles", "moves", "movie", "movies", "mummy", "music",
    "must", "mutation", "mutations", "my", "naked", "need", "never", "new", "next", "nightmare",
    "nitro", "no", "nos", "not", "note", "notes", "now", "nr", "nude", "null", "nuts", "object",
    "objectives", "objects", "of", "off", "offense", "ogre", "oil", "old", "on", "one", "one-hit",
    "only", "opponent", "or", "orange", "organs", "others", "out", "outrun", "over", "overheat",
    "override", "overtake", "own", "oxygen", "pack", "packs", "painkillers", "paint", "paper",
    "park", "partner", "parts", "passenger", "passes", "passing", "pause", "penalty", "pennant",
    "pennants", "perfect", "perm", "person", "perspective", "peter", "physics", "pick", "picks",
    "pickup", "pickups", "pig", "pills", "pimp", "piranhas", "pistol", "pistols", "pl", "planes",
    "plasma", "play", "player", "players", "plays", "plus", "pods", "point", "points", "pol",
    "potion", "potions", "power", "powers", "pressure", "primary", "prisoner", "prisoners",
    "profiles", "progress", "projectiles", "proof", "proofs", "proximity", "pts", "pulse", "punch",
    "punches", "punting", "purchases", "puzzle", "quarter", "quarters", "quest", "quick", "race",
    "races", "radar", "rage", "rail", "rain", "rally", "rank", "ranks", "ransom", "rapid", "rare",
    "rate", "rating", "ratings", "ray", "re", "read", "really", "rear", "recall", "recalls",
    "receiver", "recovery", "recruit", "red", "refill", "reflection", "relation", "release",
    "reload", "reloads", "remove", "rep", "repair", "repairs", "replays", "reputation",
    "requirement", "rescue", "reset", "resistance", "respect", "retries", "reuse", "reverse",
    "revives", "reward", "rewards", "rewinds", "rifle", "right", "rings", "rise", "rm", "roar",
    "rob", "rocket", "rockets", "rocks", "rogue", "round", "rs", "rt", "run", "running", "runs",
    "saber", "sailing", "salvage", "sand", "sands", "sandstorm", "save", "saves", "scan", "scarabs",
    "scare", "scared", "scenes", "schools", "score", "scores", "scoring", "scrap", "screen",
    "screenshot", "screws", "scrolls", "sea", "seal", "sec", "secondary", "secret", "secrets",
    "see", "select", "selector", "sell", "sense", "set", "sex", "shards", "shield", "shields",
    "shift", "ship", "ships", "shirt", "shoot", "shooter", "shop", "shopping", "short", "shot",
    "shotgun", "shots", "show", "shurikens", "silencer", "silent", "sill", "single", "sits", "size",
    "sizes", "skaters", "skill", "skills", "slaughter", "sleep", "slow", "slower", "small", "smith",
    "smoke", "snack", "snacks", "snatch", "sneak", "sniper", "snow", "snowball", "social", "soon",
    "soul", "souls", "sp", "spatula", "spcl", "spd", "spec", "special", "specials", "speed",
    "spells", "spend", "sperm", "spikes", "spin", "spirit", "spirits", "spiritual", "splints",
    "sponsors", "spray", "spur", "st", "stadiums", "stake", "stakes", "stamina", "stand", "stars",
    "start", "stat", "state", "static", "stats", "statues", "status", "stay", "stdown", "steals",
    "stealth", "stick", "sticky", "stone", "stoned", "stones", "stop", "store", "story", "street",
    "strength", "strike", "strikes", "stroke", "stronger", "strongholds", "studs", "stuff", "stunt",
    "style", "sub", "subspace", "sucks", "suicide", "suit", "super", "supply", "survival",
    "survive", "sutures", "swan", "swelling", "swim", "switch", "switching", "sword", "tactical",
    "tails", "take", "talisman", "talon", "tank", "tanks", "tape", "team", "teammate", "teammates",
    "teamwork", "tech", "temp", "tempests", "tennis", "tension", "terabytes", "terror", "test",
    "textures", "thdown", "the", "theft", "there", "third", "thorn", "thru", "thumb", "tickets",
    "tiger", "time", "timer", "timers", "timestamp", "tint", "tiny", "to", "tog", "toggle", "token",
    "tokens", "tons", "torpedoes", "total", "touch", "tracks", "trades", "traffic", "train",
    "training", "tranquility", "transfer", "transfers", "traps", "travel", "trick", "tricky",
    "trinity", "troops", "trophies", "try", "tubes", "turbo", "turn", "turret", "tux", "two",
    "ultra", "uncensored", "underwear", "units", "unl", "unleash", "unlimited", "unlimted",
    "unlock", "unlocked", "unlocks", "up", "upgrade", "upgrades", "use", "useable", "value",
    "vehicle", "vendetta", "vermin", "video", "view", "viewer", "vision", "visions", "voodoo", "vs",
    "walk", "walking", "walls", "walnuts", "wanted", "washers", "wasp", "wastes", "water", "weak",
    "weapon", "weapons", "weather", "weird", "wheel", "when", "whistle", "white", "wide", "will",
    "win", "wind", "wins", "with", "without", "won", "wont", "workout", "workshop", "xair", "xflip",
    "xlip", "xmanual", "xollie", "xp", "xrail", "xrun", "xspeed", "xspin", "xswitch", "you",
    "young", "your", "zapper", "zero", "zombies", "zone", "zoom",
};

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static bool common_word(const char *lower)
{
    return bsearch(&lower, k_common, sizeof(k_common) / sizeof(k_common[0]), sizeof(k_common[0]),
                   cmp_str) != NULL;
}

/* abbreviations and misspellings, a word each (lower case), as written out */
static const struct {
    const char *from, *to;
} k_word[] = {
    {"inf", "infinite"},       {"infinate", "infinite"},   {"infininte", "infinite"},
    {"infinte", "infinite"},   {"infinante", "infinite"},  {"infinit", "infinite"},
    {"unlimited", "infinite"}, {"unlim", "infinite"},      {"unl", "infinite"},
    {"unlimted", "infinite"},  {"unendlich", "infinite"},  {"infinity", "infinite"},
    {"hlth", "health"},        {"pts", "points"},          {"pnts", "points"},
    {"exp", "experience"},     {"lvl", "level"},           {"lvls", "levels"},
    {"weap", "weapon"},        {"weaps", "weapons"},       {"plyr", "player"},
    {"enmy", "enemy"},         {"alwys", "always"},        {"alw", "always"},
    {"dont", "don't"},         {"cant", "can't"},          {"doesnt", "doesn't"},
    {"wont", "won't"},         {"isnt", "isn't"},          {"didnt", "didn't"},
    {"missles", "missiles"},   {"misiles", "missiles"},    {"relaod", "reload"},
    {"molitov", "molotov"},    {"fatalitys", "fatalities"}, {"seconday", "secondary"},
    {"invinciblity", "invincibility"}, {"invincability", "invincibility"},
    {"lotsa", "lots of"},      {"loadsa", "lots of"},      {"spcl", "special"},
    {"spd", "speed"},          {"cht", "cheat"},           {"chars", "characters"},
    {"upg", "upgrade"},        {"upgs", "upgrades"},       {"amo", "ammo"},
    {"helth", "health"},       {"healt", "health"},        {"granades", "grenades"},
    {"ininite", "infinite"},   {"inifinite", "infinite"},  {"ininfite", "infinite"},
    {"ininiite", "infinite"},  {"unlimiited", "infinite"}, {"unlimit", "infinite"},
    {"unendl", "infinite"},    {"granade", "grenade"},     {"granades", "grenades"},
    {"nades", "grenades"},     {"lifes", "lives"},         {"enemys", "enemies"},
    {"medipack", "medkits"},   {"medipacks", "medkits"},   {"healthpacks", "health packs"},
    {"maxhelth", "max health"}, {"infhealth", "infinite health"}, {"infiniteammo", "infinite ammo"},
    {"dosnt", "doesn't"},      {"dwn", "down"},            {"difficulity", "difficulty"},
    {"idestructable", "indestructible"}, {"disintigrator", "disintegrator"}, {"boms", "bombs"},
    {"milion", "million"},     {"creds", "credits"},       {"attr", "attribute"},
    {"attrib", "attribute"},   {"armour", "armor"},        {"ammunitions", "ammo"},
    {"onehit", "one-hit"},     {"skillpts", "skill points"}, {"devpts", "dev points"},
    {"checkpt", "checkpoint"}, {"wnt", "wanted"},          {"everythings", "everything"},
    {"everyones", "everyone's"}, {"oppon", "opponent"},    {"sngl", "single"},
    {"n20", "N2O"},            {"n2o", "N2O"},             {"nos", "NOS"},
    {"hp", "HP"},              {"sp", "SP"},               {"xp", "XP"},
    {"mp", "MP"},              {"ai", "AI"},               {"cpu", "CPU"},
    {"ko", "KO"},              {"rt", "RT"},               {"lt", "LT"},
    {"rb", "RB"},              {"lb", "LB"},               {"dpad", "D-pad"},
    {"d-pad", "D-pad"},        {"$$$", "money"},           {"$$", "money"},
    {"$$$$", "money"},         {"medkit", "medkits"},      {"medkits", "medkits"},
    {"medikit", "medkits"},    {"medpacks", "medkits"},    {"medikits", "medkits"},
};

static const char *word_map(const char *lower)
{
    for (size_t i = 0; i < sizeof(k_word) / sizeof(k_word[0]); i++) {
        if (!strcmp(lower, k_word[i].from)) {
            return k_word[i].to;
        }
    }
    return NULL;
}

static void lower_into(char *d, const char *s, size_t n)
{
    size_t i = 0;
    for (; s[i] && i + 1 < n; i++) {
        d[i] = s[i] >= 'A' && s[i] <= 'Z' ? (char)(s[i] + 32) : s[i];
    }
    d[i] = 0;
}

static bool is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* ------------------------------------------------------------ buttons */

/* a button hint's token, as said in English ("" for a separator word that
 * goes, NULL when it isn't one) */
static const char *button_word(const char *lower)
{
    static const struct { const char *from, *to; } k_btn[] = {
        {"rt", "RT"}, {"lt", "LT"}, {"rb", "RB"}, {"lb", "LB"}, {"r", "right"}, {"l", "left"},
        {"u", "up"}, {"d", "down"}, {"up", "up"}, {"down", "down"}, {"left", "left"},
        {"right", "right"}, {"a", "A"}, {"b", "B"}, {"x", "X"}, {"y", "Y"}, {"white", "White"},
        {"black", "Black"}, {"back", "Back"}, {"start", "Start"}, {"dpad", "D-pad"},
        {"d-pad", "D-pad"}, {"rthumb", "RS"}, {"r-thumb", "RS"}, {"rtmb", "RS"}, {"rth", "RS"},
        {"rs", "RS"}, {"r3", "RS"}, {"lthumb", "LS"}, {"l-thumb", "LS"}, {"ltmb", "LS"},
        {"lth", "LS"}, {"ls", "LS"}, {"l3", "LS"}, {"or", "or"}, {"hold", "hold"},
        {"button", ""}, {"key", ""}, {"igk", ""}, {"igkey", ""}, {"igks", ""}, {"thumb", ""},
        {"ltrig", "LT"}, {"rtrig", "RT"}, {"lt+rt", "LT + RT"},
    };
    for (size_t i = 0; i < sizeof(k_btn) / sizeof(k_btn[0]); i++) {
        if (!strcmp(lower, k_btn[i].from)) {
            return k_btn[i].to;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------ translations */

/* es fr de it pt ru pl ja zh ko */
typedef struct {
    const char *en;
    const char *t[NT];
} Tr;

/* whole names (lower case English, as cheat_text writes them) */
static const Tr k_phrase[] = {
    {"one-hit kills", {"Matar de un golpe", "Tuer en un coup", "Ein-Treffer-Kills", "Uccisioni con un colpo",
                       "Matar com um golpe", "Убийство с одного удара", "Zabicie jednym ciosem", "一撃必殺",
                       "一击必杀", "한 방에 처치"}},
    {"one-hit KO", {"KO de un golpe", "KO en un coup", "K.o. mit einem Treffer", "KO con un colpo",
                    "Nocaute com um golpe", "Нокаут с одного удара", "Nokaut jednym ciosem", "一撃KO", "一击KO",
                    "한 방 KO"}},
    {"no reload", {"Sin recargar", "Sans rechargement", "Kein Nachladen", "Senza ricarica", "Sem recarregar",
                   "Без перезарядки", "Bez przeładowania", "リロード不要", "无需装填", "재장전 없음"}},
    {"free purchases", {"Compras gratis", "Achats gratuits", "Kostenlose Einkäufe", "Acquisti gratis",
                        "Compras grátis", "Бесплатные покупки", "Darmowe zakupy", "購入無料", "免费购买",
                        "무료 구매"}},
    {"free items", {"Objetos gratis", "Objets gratuits", "Kostenlose Gegenstände", "Oggetti gratis",
                    "Itens grátis", "Бесплатные предметы", "Darmowe przedmioty", "アイテム無料", "免费道具",
                    "무료 아이템"}},
    {"free shop items", {"Tienda gratis", "Boutique gratuite", "Kostenloser Shop", "Negozio gratis", "Loja grátis",
                         "Бесплатный магазин", "Darmowy sklep", "ショップ無料", "商店免费", "상점 무료"}},
    {"free upgrades", {"Mejoras gratis", "Améliorations gratuites", "Kostenlose Upgrades", "Potenziamenti gratis",
                       "Melhorias grátis", "Бесплатные улучшения", "Darmowe ulepszenia", "アップグレード無料",
                       "免费升级", "무료 업그레이드"}},
    {"free equipment", {"Equipo gratis", "Équipement gratuit", "Kostenlose Ausrüstung", "Equipaggiamento gratis",
                        "Equipamento grátis", "Бесплатное снаряжение", "Darmowy ekwipunek", "装備無料", "免费装备",
                        "무료 장비"}},
    {"enable all", {"Activar todo", "Tout activer", "Alles aktivieren", "Attiva tutto", "Ativar tudo",
                    "Включить всё", "Włącz wszystko", "すべて有効", "全部启用", "모두 활성화"}},
    {"unlock everything", {"Desbloquear todo", "Tout débloquer", "Alles freischalten", "Sblocca tutto",
                           "Desbloquear tudo", "Открыть всё", "Odblokuj wszystko", "すべて解放", "全部解锁",
                           "모두 해제"}},
    {"unlock all levels", {"Desbloquear todos los niveles", "Débloquer tous les niveaux", "Alle Level freischalten",
                           "Sblocca tutti i livelli", "Desbloquear todas as fases", "Открыть все уровни",
                           "Odblokuj wszystkie poziomy", "全ステージ解放", "解锁全部关卡", "모든 레벨 해제"}},
    {"unlock all cars", {"Desbloquear todos los coches", "Débloquer toutes les voitures", "Alle Autos freischalten",
                         "Sblocca tutte le auto", "Desbloquear todos os carros", "Открыть все машины",
                         "Odblokuj wszystkie samochody", "全車種解放", "解锁全部车辆", "모든 차량 해제"}},
    {"unlock all characters", {"Desbloquear todos los personajes", "Débloquer tous les personnages",
                               "Alle Figuren freischalten", "Sblocca tutti i personaggi",
                               "Desbloquear todos os personagens", "Открыть всех персонажей",
                               "Odblokuj wszystkie postacie", "全キャラクター解放", "解锁全部角色",
                               "모든 캐릭터 해제"}},
    {"unlock all missions", {"Desbloquear todas las misiones", "Débloquer toutes les missions",
                             "Alle Missionen freischalten", "Sblocca tutte le missioni",
                             "Desbloquear todas as missões", "Открыть все миссии", "Odblokuj wszystkie misje",
                             "全ミッション解放", "解锁全部任务", "모든 미션 해제"}},
    {"unlock all weapons", {"Desbloquear todas las armas", "Débloquer toutes les armes", "Alle Waffen freischalten",
                            "Sblocca tutte le armi", "Desbloquear todas as armas", "Открыть всё оружие",
                            "Odblokuj całą broń", "全武器解放", "解锁全部武器", "모든 무기 해제"}},
    {"unlock cheats", {"Desbloquear trucos", "Débloquer les codes", "Cheats freischalten", "Sblocca i trucchi",
                       "Desbloquear trapaças", "Открыть читы", "Odblokuj kody", "チート解放", "解锁秘籍",
                       "치트 해제"}},
    {"unlock extras", {"Desbloquear extras", "Débloquer les bonus", "Extras freischalten", "Sblocca gli extra",
                       "Desbloquear extras", "Открыть дополнения", "Odblokuj dodatki", "おまけ解放", "解锁额外内容",
                       "보너스 해제"}},
    {"freeze timer", {"Detener el cronómetro", "Bloquer le chrono", "Timer anhalten", "Blocca il timer",
                      "Congelar o cronômetro", "Остановить таймер", "Zatrzymaj licznik czasu", "タイマー停止",
                      "冻结计时", "타이머 정지"}},
    {"freeze time", {"Congelar el tiempo", "Figer le temps", "Zeit anhalten", "Ferma il tempo", "Congelar o tempo",
                     "Остановить время", "Zatrzymaj czas", "時間停止", "冻结时间", "시간 정지"}},
    {"no cheat counter", {"Sin contador de trucos", "Sans compteur de triche", "Kein Cheat-Zähler",
                          "Senza contatore dei trucchi", "Sem contador de trapaças", "Без счётчика читов",
                          "Bez licznika kodów", "チート回数なし", "不计作弊次数", "치트 카운터 없음"}},
    {"lots of money", {"Mucho dinero", "Beaucoup d'argent", "Viel Geld", "Tanti soldi", "Muito dinheiro",
                       "Много денег", "Dużo pieniędzy", "大金", "大量金钱", "많은 돈"}},
    {"don't spend money", {"El dinero no se gasta", "L'argent ne baisse pas", "Geld wird nicht ausgegeben",
                           "I soldi non si spendono", "O dinheiro não acaba", "Деньги не тратятся",
                           "Pieniądze się nie kończą", "お金が減らない", "金钱不减", "돈이 줄지 않음"}},
    {"invincible", {"Invencible", "Invincible", "Unverwundbar", "Invincibile", "Invencível", "Неуязвимость",
                    "Nieśmiertelność", "無敵", "无敌", "무적"}},
    {"god mode", {"Modo dios", "Mode dieu", "Gottmodus", "Modalità dio", "Modo deus", "Режим бога", "Tryb boga",
                  "ゴッドモード", "上帝模式", "갓 모드"}},
    {"never die", {"Nunca morir", "Ne jamais mourir", "Niemals sterben", "Non morire mai", "Nunca morrer",
                   "Бессмертие", "Nigdy nie umieraj", "死なない", "永不死亡", "죽지 않음"}},
    {"super speed", {"Supervelocidad", "Super vitesse", "Supergeschwindigkeit", "Super velocità", "Supervelocidade",
                     "Суперскорость", "Superprędkość", "超高速", "超级速度", "초고속"}},
    {"super jump", {"Supersalto", "Super saut", "Supersprung", "Super salto", "Super pulo", "Суперпрыжок",
                    "Superskok", "スーパージャンプ", "超级跳跃", "슈퍼 점프"}},
    {"moon jump", {"Salto lunar", "Saut lunaire", "Mondsprung", "Salto lunare", "Pulo lunar", "Лунный прыжок",
                   "Księżycowy skok", "ムーンジャンプ", "月球跳跃", "문 점프"}},
    {"moon physics", {"Gravedad lunar", "Gravité lunaire", "Mondschwerkraft", "Gravità lunare", "Gravidade lunar",
                      "Лунная гравитация", "Grawitacja księżycowa", "月面重力", "月球重力", "달 중력"}},
    {"rapid fire", {"Disparo rápido", "Tir rapide", "Schnellfeuer", "Fuoco rapido", "Tiro rápido",
                    "Скорострельность", "Szybki ogień", "連射", "快速射击", "연사"}},
    {"kill all", {"Matar a todos", "Tuer tout le monde", "Alle töten", "Uccidi tutti", "Matar todos", "Убить всех",
                  "Zabij wszystkich", "全員撃破", "全部消灭", "모두 처치"}},
    {"no damage", {"Sin daño", "Aucun dégât", "Kein Schaden", "Nessun danno", "Sem dano", "Без урона",
                   "Brak obrażeń", "ダメージなし", "无伤害", "피해 없음"}},
    {"no vehicle damage", {"Sin daños al vehículo", "Véhicule indestructible", "Kein Fahrzeugschaden",
                           "Nessun danno al veicolo", "Sem dano ao veículo", "Машина без повреждений",
                           "Brak uszkodzeń pojazdu", "車両ダメージなし", "车辆无损伤", "차량 피해 없음"}},
    {"always first", {"Siempre primero", "Toujours premier", "Immer Erster", "Sempre primo", "Sempre em primeiro",
                      "Всегда первый", "Zawsze pierwszy", "常に1位", "总是第一", "항상 1위"}},
    {"level select", {"Selección de nivel", "Sélection du niveau", "Levelauswahl", "Selezione livello",
                      "Seleção de fase", "Выбор уровня", "Wybór poziomu", "ステージセレクト", "关卡选择",
                      "레벨 선택"}},
    {"double damage", {"Daño doble", "Dégâts doublés", "Doppelter Schaden", "Danno doppio", "Dano dobrado",
                       "Двойной урон", "Podwójne obrażenia", "ダメージ2倍", "双倍伤害", "피해 2배"}},
    {"no overheat", {"Sin sobrecalentamiento", "Pas de surchauffe", "Keine Überhitzung", "Niente surriscaldamento",
                     "Sem superaquecimento", "Без перегрева", "Bez przegrzewania", "オーバーヒートなし", "不会过热",
                     "과열 없음"}},
    {"weak enemies", {"Enemigos débiles", "Ennemis faibles", "Schwache Gegner", "Nemici deboli", "Inimigos fracos",
                      "Слабые враги", "Słabi wrogowie", "敵が弱い", "敌人变弱", "약한 적"}},
    {"wanted level", {"Nivel de búsqueda", "Niveau de recherche", "Fahndungslevel", "Livello di ricercato",
                      "Nível de procurado", "Уровень розыска", "Poziom poszukiwania", "手配レベル", "通缉等级",
                      "수배 레벨"}},
    {"fly", {"Volar", "Voler", "Fliegen", "Volare", "Voar", "Полёт", "Latanie", "飛行", "飞行", "비행"}},
    {"big heads", {"Cabezas grandes", "Grosses têtes", "Große Köpfe", "Teste grandi", "Cabeças grandes",
                   "Большие головы", "Duże głowy", "ビッグヘッド", "大头模式", "큰 머리"}},
    {"no timer", {"Sin cronómetro", "Sans chrono", "Kein Timer", "Senza timer", "Sem cronômetro", "Без таймера",
                  "Bez limitu czasu", "時間制限なし", "无时间限制", "시간 제한 없음"}},
    {"invisible", {"Invisible", "Invisible", "Unsichtbar", "Invisibile", "Invisível", "Невидимость", "Niewidzialność",
                   "透明化", "隐身", "투명"}},
    {"no heat", {"Sin persecución", "Pas de recherche", "Keine Fahndung", "Nessun inseguimento", "Sem perseguição",
                 "Без погони", "Bez pościgu", "追跡なし", "无追捕", "추격 없음"}},
    {"read note", {"ver nota", "voir la note", "siehe Hinweis", "vedi nota", "ver nota", "см. примечание",
                   "zob. uwagę", "注記あり", "见说明", "설명 참고"}},
    {"auto block", {"Bloqueo automático", "Blocage automatique", "Automatisches Blocken", "Parata automatica",
                    "Bloqueio automático", "Автоблок", "Automatyczny blok", "オートガード", "自动格挡",
                    "자동 방어"}},
};

/* "same as": names written other ways */
static const struct {
    const char *en, *as;
} k_alias[] = {
    {"one hit kill", "one-hit kills"},      {"one-hit kill", "one-hit kills"},
    {"easy kill", "weak enemies"},          {"lots of cash", "lots of money"},
    {"lots of cash", "lots of money"},      {"lots of money", "lots of money"},
    {"tons of money", "lots of money"},     {"purchases free", "free purchases"},
    {"lock timer", "freeze timer"},         {"stop timer", "freeze timer"},
    {"freeze clock", "freeze timer"},       {"disable timer", "freeze timer"},
    {"lock clock", "freeze timer"},         {"stop clock", "freeze timer"},
    {"timer freeze", "freeze timer"},       {"freeze timers", "freeze timer"},
    {"frozen timers", "freeze timer"},      {"stop time", "freeze time"},
    {"don't spend cash", "don't spend money"}, {"money does not decrease", "don't spend money"},
    {"invincibility", "invincible"},        {"unlock all", "unlock everything"},
    {"super unlock", "unlock everything"},  {"unlock all cheats", "unlock cheats"},
    {"enable all hacks", "enable all"},     {"no reloads", "no reload"},
    {"never reload", "no reload"},          {"no car damage", "no vehicle damage"},
    {"always 1st", "always first"},         {"always finish 1st", "always first"},
    {"free shop", "free shop items"},       {"items free", "free items"},
    {"everything is free", "free purchases"}, {"moon gravity", "moon physics"},
    {"no weapon overheat", "no overheat"},  {"big head", "big heads"},
    {"big heads toggle", "big heads"},      {"zero heat", "no heat"},
};

/* things that are infinite or at their most: the noun, its gender (m f n,
 * upper case for a plural) first; Japanese, Chinese and Korean with '-' */
static const Tr k_noun[] = {
    {"health", {"fvida", "fvie", "fGesundheit", "fsalute", "fvida", "nздоровье", "nzdrowie", "-体力", "-生命", "-체력"}},
    {"life", {"fvida", "fvie", "nLeben", "fvita", "fvida", "fжизнь", "nżycie", "-ライフ", "-生命", "-생명"}},
    {"lives", {"Fvidas", "Fvies", "NLeben", "Fvite", "Fvidas", "Nжизни", "Nżycia", "-残機", "-命数", "-목숨"}},
    {"ammo", {"fmunición", "Fmunitions", "fMunition", "Fmunizioni", "fmunição", "Nпатроны", "famunicja", "-弾薬",
              "-弹药", "-탄약"}},
    {"grenades", {"Fgranadas", "Fgrenades", "NGranaten", "Fgranate", "Fgranadas", "Nгранаты", "Ngranaty",
                  "-グレネード", "-手雷", "-수류탄"}},
    {"items", {"Mobjetos", "Mobjets", "NGegenstände", "Moggetti", "Mitens", "Nпредметы", "Nprzedmioty", "-アイテム",
               "-道具", "-아이템"}},
    {"air", {"maire", "mair", "fLuft", "faria", "mar", "mвоздух", "npowietrze", "-酸素", "-氧气", "-산소"}},
    {"air supply", {"maire", "mair", "fLuft", "faria", "mar", "mвоздух", "npowietrze", "-酸素", "-氧气", "-산소"}},
    {"oxygen", {"moxígeno", "moxygène", "mSauerstoff", "mossigeno", "moxigênio", "mкислород", "mtlen", "-酸素",
                "-氧气", "-산소"}},
    {"special", {"fbarra especial", "fjauge spéciale", "fSpezialleiste", "fbarra speciale", "mespecial",
                 "fспецшкала", "mpasek specjalny", "-スペシャル", "-特殊技", "-스페셜"}},
    {"specials", {"Mespeciales", "Mspéciaux", "NSpezialangriffe", "Mspeciali", "Mespeciais", "Nспецприёмы",
                  "Nataki specjalne", "-必殺技", "-特殊技", "-필살기"}},
    {"armor", {"farmadura", "farmure", "fRüstung", "farmatura", "farmadura", "fброня", "mpancerz", "-アーマー",
               "-护甲", "-아머"}},
    {"nitro", {"mnitro", "mnitro", "nNitro", "mnitro", "mnitro", "nнитро", "nnitro", "-ニトロ", "-氮气", "-니트로"}},
    {"N2O", {"mnitro", "mnitro", "nNitro", "mnitro", "mnitro", "nнитро", "nnitro", "-ニトロ", "-氮气", "-니트로"}},
    {"NOS", {"mnitro", "mnitro", "nNitro", "mnitro", "mnitro", "nнитро", "nnitro", "-ニトロ", "-氮气", "-니트로"}},
    {"stamina", {"fresistencia", "fendurance", "fAusdauer", "fresistenza", "festamina", "fвыносливость",
                 "fwytrzymałość", "-スタミナ", "-耐力", "-스태미나"}},
    {"endurance", {"fresistencia", "fendurance", "fAusdauer", "fresistenza", "fresistência", "fвыносливость",
                   "fwytrzymałość", "-スタミナ", "-耐力", "-지구력"}},
    {"time", {"mtiempo", "mtemps", "fZeit", "mtempo", "mtempo", "nвремя", "mczas", "-時間", "-时间", "-시간"}},
    {"money", {"mdinero", "margent", "nGeld", "Msoldi", "mdinheiro", "Nденьги", "Npieniądze", "-お金", "-金钱", "-돈"}},
    {"cash", {"mdinero", "margent", "nGeld", "Msoldi", "mdinheiro", "Nденьги", "Npieniądze", "-お金", "-金钱", "-돈"}},
    {"gold", {"moro", "mor", "nGold", "moro", "mouro", "nзолото", "nzłoto", "-ゴールド", "-金币", "-골드"}},
    {"coins", {"Fmonedas", "Fpièces", "NMünzen", "Fmonete", "Fmoedas", "Nмонеты", "Nmonety", "-コイン", "-硬币",
               "-코인"}},
    {"boost", {"mturbo", "mturbo", "mTurbo", "mturbo", "mturbo", "nускорение", "mdopalacz", "-ブースト", "-加速",
               "-부스트"}},
    {"turbo", {"mturbo", "mturbo", "mTurbo", "mturbo", "mturbo", "nускорение", "mdopalacz", "-ターボ", "-涡轮",
               "-터보"}},
    {"arrows", {"Fflechas", "Fflèches", "NPfeile", "Ffrecce", "Fflechas", "Nстрелы", "Nstrzały", "-矢", "-箭矢",
                "-화살"}},
    {"clip", {"mcargador", "mchargeur", "nMagazin", "mcaricatore", "mpente", "fобойма", "mmagazynek", "-マガジン",
              "-弹匣", "-탄창"}},
    {"magic", {"fmagia", "fmagie", "fMagie", "fmagia", "fmagia", "fмагия", "fmagia", "-魔力", "-魔力", "-마력"}},
    {"mana", {"mmaná", "mmana", "nMana", "mmana", "fmana", "fмана", "fmana", "-マナ", "-法力", "-마나"}},
    {"shield", {"mescudo", "mbouclier", "mSchild", "mscudo", "mescudo", "mщит", "ftarcza", "-シールド", "-护盾",
                "-실드"}},
    {"fuel", {"mcombustible", "mcarburant", "mTreibstoff", "mcarburante", "mcombustível", "nтопливо", "npaliwo",
              "-燃料", "-燃料", "-연료"}},
    {"energy", {"fenergía", "fénergie", "fEnergie", "fenergia", "fenergia", "fэнергия", "fenergia", "-エネルギー",
                "-能量", "-에너지"}},
    {"focus", {"fconcentración", "fconcentration", "mFokus", "fconcentrazione", "mfoco", "fконцентрация",
               "nskupienie", "-集中力", "-专注", "-집중력"}},
    {"power", {"mpoder", "fpuissance", "fKraft", "mpotere", "mpoder", "fсила", "fmoc", "-パワー", "-力量", "-파워"}},
    {"powers", {"Mpoderes", "Mpouvoirs", "NKräfte", "Mpoteri", "Mpoderes", "Nспособности", "Nmoce", "-能力",
                "-能力", "-능력"}},
    {"missiles", {"Mmisiles", "Mmissiles", "NRaketen", "Mmissili", "Mmísseis", "Nракеты", "Npociski", "-ミサイル",
                  "-导弹", "-미사일"}},
    {"rockets", {"Mcohetes", "Froquettes", "NRaketen", "Mrazzi", "Mfoguetes", "Nракеты", "Nrakiety", "-ロケット",
                 "-火箭", "-로켓"}},
    {"bullets", {"Fbalas", "Fballes", "NKugeln", "Mproiettili", "Fbalas", "Nпули", "Nnaboje", "-弾", "-子弹",
                 "-총알"}},
    {"bombs", {"Fbombas", "Fbombes", "NBomben", "Fbombe", "Fbombas", "Nбомбы", "Nbomby", "-ボム", "-炸弹", "-폭탄"}},
    {"potions", {"Fpociones", "Fpotions", "NTränke", "Fpozioni", "Fpoções", "Nзелья", "Nmikstury", "-ポーション",
                 "-药水", "-물약"}},
    {"experience", {"fexperiencia", "fexpérience", "fErfahrung", "fesperienza", "fexperiência", "mопыт",
                    "ndoświadczenie", "-経験値", "-经验值", "-경험치"}},
    {"XP", {"fexperiencia", "fexpérience", "fErfahrung", "fesperienza", "fexperiência", "mопыт", "ndoświadczenie",
            "-経験値", "-经验值", "-경험치"}},
    {"skill points", {"Mpuntos de habilidad", "Mpoints de compétence", "NFähigkeitspunkte", "Mpunti abilità",
                      "Mpontos de habilidade", "Nочки навыков", "Npunkty umiejętności", "-スキルポイント",
                      "-技能点", "-스킬 포인트"}},
    {"points", {"Mpuntos", "Mpoints", "NPunkte", "Mpunti", "Mpontos", "Nочки", "Npunkty", "-ポイント", "-点数",
                "-포인트"}},
    {"continues", {"Fcontinuaciones", "Mcontinues", "NContinues", "Mcontinue", "Mcontinues", "Nпродолжения",
                   "Nkontynuacje", "-コンティニュー", "-接关次数", "-컨티뉴"}},
    {"credits", {"Mcréditos", "Mcrédits", "NCredits", "Mcrediti", "Mcréditos", "Nкредиты", "Nkredyty",
                 "-クレジット", "-信用点", "-크레딧"}},
    {"saves", {"Mguardados", "Fsauvegardes", "NSpeicherungen", "Msalvataggi", "Msalvamentos", "Nсохранения",
               "Nzapisy", "-セーブ回数", "-存档次数", "-저장 횟수"}},
    {"rage", {"ffuria", "frage", "fWut", "ffuria", "ffúria", "fярость", "ffuria", "-レイジ", "-怒气", "-분노"}},
    {"weapons", {"Farmas", "Farmes", "NWaffen", "Farmi", "Farmas", "nоружие", "fbroń", "-武器", "-武器", "-무기"}},
    {"upgrades", {"Fmejoras", "Faméliorations", "NUpgrades", "Mpotenziamenti", "Fmelhorias", "Nулучшения",
                  "Nulepszenia", "-アップグレード", "-升级", "-업그레이드"}},
    {"keys", {"Fllaves", "Fclés", "NSchlüssel", "Fchiavi", "Fchaves", "Nключи", "Nklucze", "-鍵", "-钥匙", "-열쇠"}},
    {"medkits", {"Mbotiquines", "Ftrousses de soin", "NMedipacks", "Mmedikit", "Mkits médicos", "Nаптечки",
                 "Napteczki", "-回復アイテム", "-医疗包", "-구급상자"}},
    {"health packs", {"Mbotiquines", "Ftrousses de soin", "NMedipacks", "Mmedikit", "Mkits médicos", "Nаптечки",
                      "Napteczki", "-回復アイテム", "-医疗包", "-구급상자"}},
    {"sand", {"farena", "msable", "mSand", "fsabbia", "fareia", "mпесок", "mpiasek", "-砂", "-沙", "-모래"}},
    {"sands", {"farena", "msable", "mSand", "fsabbia", "fareia", "mпесок", "mpiasek", "-砂", "-沙", "-모래"}},
    {"stats", {"Festadísticas", "Fstatistiques", "NWerte", "Fstatistiche", "Matributos", "Nхарактеристики",
               "Nstatystyki", "-ステータス", "-属性", "-능력치"}},
    {"level", {"mnivel", "mniveau", "nLevel", "mlivello", "mnível", "mуровень", "mpoziom", "-レベル", "-等级",
               "-레벨"}},
    {"respect", {"mrespeto", "mrespect", "mRespekt", "mrispetto", "mrespeito", "nуважение", "mszacunek",
                 "-リスペクト", "-声望", "-리스펙트"}},
    {"lasers", {"Mláseres", "Mlasers", "NLaser", "Mlaser", "Mlasers", "Nлазеры", "Nlasery", "-レーザー", "-激光",
                "-레이저"}},
    {"flashbangs", {"Fgranadas cegadoras", "Fgrenades aveuglantes", "NBlendgranaten", "Fgranate accecanti",
                    "Fgranadas de luz", "Nсветошумовые гранаты", "Ngranaty hukowe", "-閃光弾", "-闪光弹",
                    "-섬광탄"}},
};

/* "infinite" by gender (m f n M F N) */
static const char *const k_inf[NT][6] = {
    {"infinito", "infinita", "infinito", "infinitos", "infinitas", "infinitos"},
    {"infini", "infinie", "infini", "infinis", "infinies", "infinis"},
    {"Unendlicher", "Unendliche", "Unendliches", "Unendliche", "Unendliche", "Unendliche"},
    {"infinito", "infinita", "infinito", "infiniti", "infinite", "infiniti"},
    {"infinito", "infinita", "infinito", "infinitos", "infinitas", "infinitos"},
    {"Бесконечный", "Бесконечная", "Бесконечное", "Бесконечные", "Бесконечные", "Бесконечные"},
    {"Nieskończony", "Nieskończona", "Nieskończone", "Nieskończone", "Nieskończone", "Nieskończone"},
    {"", "", "", "", "", ""},
    {"", "", "", "", "", ""},
    {"", "", "", "", "", ""},
};

/* "<the thing> at its most", "%s" the noun */
static const char *const k_max[NT] = {
    "%s al máximo", "%s au maximum", "Max. %s", "%s al massimo", "%s no máximo",
    "Макс. %s", "Maks. %s", "%s最大", "最大%s", "최대 %s",
};

static const char *const k_player[NT] = {
    "jugador %d", "joueur %d", "Spieler %d", "giocatore %d", "jogador %d",
    "игрок %d", "gracz %d", "プレイヤー%d", "玩家%d", "플레이어 %d",
};

static const char *const k_hotkey[NT] = {
    "atajo", "raccourci", "Hotkey", "scorciatoia", "atalho",
    "горячая клавиша", "skrót", "ホットキー", "热键", "단축키",
};

static const struct {
    const char *en;
    const char *t[NT];
} k_btnword[] = {
    {"up", {"arriba", "haut", "oben", "su", "cima", "вверх", "góra", "上", "上", "위"}},
    {"down", {"abajo", "bas", "unten", "giù", "baixo", "вниз", "dół", "下", "下", "아래"}},
    {"left", {"izquierda", "gauche", "links", "sinistra", "esquerda", "влево", "lewo", "左", "左", "왼쪽"}},
    {"right", {"derecha", "droite", "rechts", "destra", "direita", "вправо", "prawo", "右", "右", "오른쪽"}},
    {"or", {"o", "ou", "oder", "o", "ou", "или", "lub", "または", "或", "또는"}},
    {"hold", {"mantener", "maintenir", "halten", "tieni", "segurar", "удерживать", "przytrzymaj", "長押し", "按住",
              "길게"}},
    {"D-pad", {"cruceta", "croix", "Steuerkreuz", "croce", "direcional", "крестовина", "krzyżak", "十字キー",
               "方向键", "방향키"}},
};

/* ------------------------------------------------------------ helpers */

static void cat(char *out, size_t n, const char *s)
{
    const size_t l = strlen(out);
    if (l + 1 < n) {
        snprintf(out + l, n - l, "%s", s);
    }
}

/* s with its first letter upper case (ASCII and Latin-1's accented ones) */
static void cap_first(char *s)
{
    if (s[0] >= 'a' && s[0] <= 'z') {
        s[0] = (char)(s[0] - 32);
    } else if ((unsigned char)s[0] == 0xC3 && (unsigned char)s[1] >= 0xA0 && (unsigned char)s[1] <= 0xBE &&
               (unsigned char)s[1] != 0xB7) {
        s[1] = (char)(s[1] - 0x20);
    }
}

static const char *alias_of(const char *key)
{
    for (size_t i = 0; i < sizeof(k_alias) / sizeof(k_alias[0]); i++) {
        if (!strcmp(key, k_alias[i].en)) {
            return k_alias[i].as;
        }
    }
    return key;
}

static int ci_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        const char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        const char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y || !x) {
            return x - y;
        }
    }
}

static const Tr *find(const Tr *t, size_t nt, const char *key)
{
    for (size_t i = 0; i < nt; i++) {
        if (!ci_cmp(t[i].en, key)) {
            return &t[i];
        }
    }
    return NULL;
}

#define NPHRASE (sizeof(k_phrase) / sizeof(k_phrase[0]))
#define NNOUN (sizeof(k_noun) / sizeof(k_noun[0]))

/* the first letter lower case (inside brackets): ASCII, Latin-1, Cyrillic */
static void low_first(char *s)
{
    unsigned char *u = (unsigned char *)s;
    if (u[0] >= 'A' && u[0] <= 'Z') {
        u[0] = (unsigned char)(u[0] + 32);
    } else if (u[0] == 0xC3 && u[1] >= 0x80 && u[1] <= 0x9E && u[1] != 0x97) {
        u[1] = (unsigned char)(u[1] + 0x20);
    } else if (u[0] == 0xD0 && u[1] >= 0x90 && u[1] <= 0x9F) {
        u[1] = (unsigned char)(u[1] + 0x20);
    } else if (u[0] == 0xD0 && u[1] >= 0xA0 && u[1] <= 0xAF) {
        u[0] = 0xD1;
        u[1] = (unsigned char)(u[1] - 0x20);
    }
}

/* the name (lower-case English, a k_phrase or "infinite"/"max" and a
 * k_noun) in language li (0: Spanish ... 9: Korean); false if unknown */
static bool tr_core(const char *key0, int li, char *out, size_t n)
{
    const char *key = alias_of(key0);
    const Tr *p = find(k_phrase, NPHRASE, key);
    if (p) {
        snprintf(out, n, "%s", p->t[li]);
        return true;
    }
    const char *rest = NULL;
    bool max = false;
    if (!strncmp(key, "infinite ", 9)) {
        rest = key + 9;
    } else if (!strncmp(key, "max ", 4) || !strncmp(key, "maximum ", 8)) {
        rest = strchr(key, ' ') + 1;
        max = true;
    }
    const Tr *w = rest ? find(k_noun, NNOUN, rest) : NULL;
    if (!w && rest && rest[0] && rest[strlen(rest) - 1] != 's') {
        char pl[96];    /* "grenade": the table's "grenades" */
        snprintf(pl, sizeof(pl), "%ss", rest);
        w = find(k_noun, NNOUN, pl);
    }
    if (!w) {
        return false;
    }
    const char *e = w->t[li];
    static const char k_g[] = "mfnMFN";
    const char *gp = strchr(k_g, e[0]);
    const int g = gp && e[0] ? (int)(gp - k_g) : 0;
    char noun[96];
    snprintf(noun, sizeof(noun), "%s", e + 1);
    const int lang = li + 1;
    const bool noun_first = lang == L_ES || lang == L_FR || lang == L_IT || lang == L_PT;
    if (noun_first) {
        cap_first(noun);
    }
    if (max) {
        snprintf(out, n, k_max[li], noun);
    } else if (noun_first) {
        snprintf(out, n, "%s %s", noun, k_inf[li][g]);
    } else if (lang == L_DE || lang == L_RU || lang == L_PL) {
        snprintf(out, n, "%s %s", k_inf[li][g], noun);
    } else if (lang == L_JA) {
        snprintf(out, n, "%s無限", noun);
    } else if (lang == L_ZH) {
        snprintf(out, n, "无限%s", noun);
    } else {
        snprintf(out, n, "무한 %s", noun);
    }
    return true;
}

/* ------------------------------------------------------------ reading */

#define MAXW 24
#define WLEN 48

typedef struct {
    char w[MAXW][WLEN];
    bool mapped[MAXW];
    int n;
    int player;         /* 1-4, 0 none */
    bool hotkey, noreload;
} Words;

static bool player_word(const char *lc, int *pl)
{
    const char *d = NULL;
    if (lc[0] == 'p' && lc[1] && is_digit(lc[1]) && !lc[2]) {
        d = lc + 1;
    } else if (!strncmp(lc, "pl", 2) && is_digit(lc[2]) && !lc[3]) {
        d = lc + 2;
    } else if (!strncmp(lc, "player", 6) && is_digit(lc[6]) && !lc[7]) {
        d = lc + 6;
    } else if (is_digit(lc[0]) && lc[1] == 'p' && !lc[2]) {
        d = lc;
    }
    if (d && *d >= '1' && *d <= '4') {
        *pl = *d - '0';
        return true;
    }
    return false;
}

/* s cut at sep into its non-empty pieces (at most max) */
static int split(char *s, char sep, char **out, int max)
{
    int n = 0;
    for (char *p = s; *p && n < max;) {
        while (*p == sep) {
            p++;
        }
        if (!*p) {
            break;
        }
        out[n++] = p;
        while (*p && *p != sep) {
            p++;
        }
        if (*p) {
            *p++ = 0;
        }
    }
    return n;
}

/* the words of text (no brackets) into ws, spelled out */
static void read_words(const char *text, Words *ws)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", text);
    char *tok[MAXW];
    char raw[MAXW][WLEN];
    const int nr = split(buf, ' ', tok, MAXW);
    for (int i = 0; i < nr; i++) {
        snprintf(raw[i], WLEN, "%s", tok[i]);
    }
    for (int i = 0; i < nr; i++) {
        char w[WLEN], lc[WLEN];
        snprintf(w, sizeof(w), "%s", raw[i]);
        /* its trailing punctuation apart */
        size_t l = strlen(w);
        char trail[8] = "";
        while (l > 1 && strchr(".,:;!?", w[l - 1]) && strlen(trail) < 6) {
            memmove(trail + 1, trail, strlen(trail) + 1);
            trail[0] = w[l - 1];
            w[--l] = 0;
        }
        lower_into(lc, w, sizeof(lc));
        /* "/NR", "NR", "NoReload": no reload, said after */
        const size_t ll = strlen(lc);
        if (ll > 3 && !strcmp(lc + ll - 3, "/nr")) {
            w[ll - 3] = lc[ll - 3] = 0;
            ws->noreload = true;
        }
        if (!strcmp(lc, "nr") || !strcmp(lc, "noreload") || !strcmp(lc, "no-reload")) {
            ws->noreload = true;
            continue;
        }
        int pl;
        if (player_word(lc, &pl)) {
            ws->player = pl;
            continue;
        }
        if ((!strcmp(lc, "player") || !strcmp(lc, "pl") || !strcmp(lc, "p")) && i + 1 < nr &&
            raw[i + 1][0] >= '1' && raw[i + 1][0] <= '4' && (!raw[i + 1][1] || raw[i + 1][1] == ':')) {
            ws->player = raw[i + 1][0] - '0';
            i++;
            continue;
        }
        if (!strcmp(lc, "igk") || !strcmp(lc, "igks") || !strcmp(lc, "igkey")) {
            ws->hotkey = true;
            continue;
        }
        if (!lc[0] || ws->n >= MAXW) {
            continue;
        }
        /* each part between slashes spelled out ("Exp/Lvl") */
        char outw[WLEN * 2] = "";
        bool mapped = false;
        char parts[WLEN], *pv[8];
        snprintf(parts, sizeof(parts), "%s", w);
        bool first = true;
        const int np = split(parts, '/', pv, 8);
        for (int pi = 0; pi < np; pi++) {
            const char *pt = pv[pi];
            char pl2[WLEN];
            lower_into(pl2, pt, sizeof(pl2));
            const char *m = word_map(pl2);
            if (!first) {
                cat(outw, sizeof(outw), "/");
            }
            cat(outw, sizeof(outw), m ? m : pt);
            mapped |= m != NULL;
            first = false;
        }
        if (w[0] == '/' || !outw[0]) {
            snprintf(outw, sizeof(outw), "%s", w);
        }
        /* an abbreviation's full stop goes with it ("Inf.") */
        if (mapped && trail[0] == '.') {
            memmove(trail, trail + 1, strlen(trail));
        }
        cat(outw, sizeof(outw), trail);
        snprintf(ws->w[ws->n], WLEN, "%s", outw);
        ws->mapped[ws->n++] = mapped;
    }
    /* a capital I first, a thing after it: "I Health", "I Bandage" */
    if (ws->n > 1 && !strcmp(ws->w[0], "I")) {
        snprintf(ws->w[0], WLEN, "infinite");
        ws->mapped[0] = true;
    }
    /* "... no reload" at the end: said after, as the others' "(NR)" */
    if (ws->n > 2) {
        char a[WLEN], b[WLEN];
        lower_into(a, ws->w[ws->n - 2], sizeof(a));
        lower_into(b, ws->w[ws->n - 1], sizeof(b));
        if (!strcmp(a, "no") && (!strcmp(b, "reload") || !strcmp(b, "reloads"))) {
            ws->n -= 2;
            ws->noreload = true;
        }
    }
    /* one hit kill(s), 1 hit 1 kill, 1 hit KO: one-hit kills, one-hit KO */
    for (int i = 0; i + 2 < ws->n; i++) {
        char a[WLEN], b[WLEN], c[WLEN], d[WLEN] = "";
        lower_into(a, ws->w[i], sizeof(a));
        lower_into(b, ws->w[i + 1], sizeof(b));
        lower_into(c, ws->w[i + 2], sizeof(c));
        if (i + 3 < ws->n) {
            lower_into(d, ws->w[i + 3], sizeof(d));
        }
        if ((strcmp(a, "1") && strcmp(a, "one")) || strcmp(b, "hit")) {
            continue;
        }
        int drop = 0;
        const char *what = NULL;
        if (!strcmp(c, "kill") || !strcmp(c, "kills")) {
            drop = 1, what = "kills";
        } else if (!strcmp(c, "ko")) {
            drop = 1, what = "KO";
        } else if ((!strcmp(c, "1") || !strcmp(c, "one")) && (!strcmp(d, "kill") || !strcmp(d, "kills"))) {
            drop = 2, what = "kills";
        }
        if (!what) {
            continue;
        }
        snprintf(ws->w[i], WLEN, "one-hit");
        ws->mapped[i] = true;
        snprintf(ws->w[i + 1], WLEN, "%s", what);
        ws->mapped[i + 1] = true;
        for (int k = i + 2; k + drop < ws->n; k++) {
            memcpy(ws->w[k], ws->w[k + drop], WLEN);
            ws->mapped[k] = ws->mapped[k + drop];
        }
        ws->n -= drop;
    }
}

/* the words as a name: sentence case, the trainers' common words lower
 * case, acronyms and a game's own names as written */
static void join_words(const Words *ws, bool all_caps, char *out, size_t n)
{
    out[0] = 0;
    for (int i = 0; i < ws->n; i++) {
        char w[WLEN];
        snprintf(w, sizeof(w), "%s", ws->w[i]);
        if (!ws->mapped[i]) {
            char lc[WLEN];
            lower_into(lc, w, sizeof(lc));
            int up = 0, low = 0, dig = 0, inner_up = 0;
            for (int k = 0; w[k]; k++) {
                up += w[k] >= 'A' && w[k] <= 'Z';
                low += w[k] >= 'a' && w[k] <= 'z';
                dig += is_digit(w[k]);
                inner_up += k > 0 && w[k] >= 'A' && w[k] <= 'Z' && is_alpha(w[k - 1]) && w[k - 1] >= 'a';
            }
            char core[WLEN];
            snprintf(core, sizeof(core), "%s", lc);
            for (size_t l = strlen(core); l && !is_alpha(core[l - 1]) && core[l - 1] != '\''; l--) {
                core[l - 1] = 0;
            }
            if (all_caps) {
                snprintf(w, sizeof(w), "%s", lc);
            } else if (dig || inner_up || (up >= 2 && !low && !common_word(core))) {
                /* "SS2", "N2O", "SpeedBreaker", "RESV": as written */
            } else if (common_word(core)) {
                snprintf(w, sizeof(w), "%s", lc);
            }
        }
        if (!w[0]) {
            continue;
        }
        if (out[0]) {
            cat(out, n, " ");
        }
        cat(out, n, w);
    }
    /* no dangling dashes, slashes or colons at either end */
    size_t l = strlen(out);
    while (l && strchr(" -/:,&+", out[l - 1])) {
        out[--l] = 0;
    }
    size_t s = strspn(out, " -/:,&+");
    memmove(out, out + s, strlen(out + s) + 1);
}

/* a bracket's buttons ("Rt+U", "Lt+L or R", "R-Thumb"), in English or
 * lang; false when it isn't buttons */
static bool buttons_text(const char *note, int lang, bool *hotkey, char *out, size_t n)
{
    char b[96], lc[96];
    snprintf(b, sizeof(b), "%s", note);
    lower_into(lc, b, sizeof(lc));
    out[0] = 0;
    int real = 0;
    size_t i = 0;
    char sep = 0;
    while (lc[i]) {
        if (strchr("+/ ,", lc[i])) {
            sep = lc[i] == ',' ? ' ' : (sep == '+' || sep == '/') ? sep : lc[i];
            i++;
            continue;
        }
        size_t j = i;
        while (lc[j] && !strchr("+/ ,", lc[j])) {
            j++;
        }
        char tok[32];
        snprintf(tok, sizeof(tok), "%.*s", (int)(j - i), lc + i);
        /* "igk-back": a hotkey, then its button */
        const char *bw = button_word(tok);
        if (!bw && !strncmp(tok, "igk-", 4)) {
            *hotkey = true;
            bw = button_word(tok + 4);
        }
        if (!bw) {
            return false;
        }
        if (!strcmp(tok, "igk") || !strcmp(tok, "igkey") || !strcmp(tok, "igks")) {
            *hotkey = true;
        }
        if (bw[0]) {
            const char *say = bw;
            if (lang != L_EN) {
                for (size_t k = 0; k < sizeof(k_btnword) / sizeof(k_btnword[0]); k++) {
                    if (!strcmp(bw, k_btnword[k].en)) {
                        say = k_btnword[k].t[lang - 1];
                    }
                }
            }
            if (out[0]) {
                cat(out, n, sep == '+' ? " + " : sep == '/' ? "/" : " ");
            }
            cat(out, n, say);
            real += strcmp(bw, "or") && strcmp(bw, "hold");
        }
        sep = 0;
        i = j;
    }
    return real > 0 || (*hotkey && !out[0]);
}

void cheat_text(const char *raw, int lang, char *out, size_t n)
{
    out[0] = 0;
    if (lang < L_EN || lang >= L_COUNT) {
        lang = L_EN;
    }
    /* printable ASCII only; brackets as round ones, a space before them */
    char s[256];
    size_t k = 0;
    for (const unsigned char *p = (const unsigned char *)raw; *p && k + 2 < sizeof(s); p++) {
        unsigned char c = *p;
        c = c == '_' || c == '\t' ? ' ' : c == '[' || c == '{' ? '(' : c == ']' || c == '}' ? ')' : c;
        if (c == '*' && is_digit((char)p[1])) {
            c = 'x';        /* "POINT*5": times five */
        }
        if (c < 0x20 || c > 0x7E || strchr("*~^=|`\"#\\<>", c)) {
            continue;
        }
        if (c == '(' && k && s[k - 1] != ' ') {
            s[k++] = ' ';
        }
        if (c == ' ' && k && s[k - 1] == ' ') {
            continue;
        }
        s[k++] = (char)c;
    }
    s[k] = 0;
    /* the brackets apart */
    char text[256] = "", notes[4][96];
    int nn = 0, depth = 0;
    size_t tl = 0, nl = 0;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '(') {
            if (!depth++ && nn < 4) {
                nl = 0;
            }
            continue;
        }
        if (s[i] == ')' && depth) {
            if (!--depth && nn < 4) {
                notes[nn][nl] = 0;
                nn += nl > 0;
            }
            continue;
        }
        if (depth) {
            if (nn < 4 && nl + 1 < sizeof(notes[0])) {
                notes[nn][nl++] = s[i];
            }
        } else if (tl + 1 < sizeof(text)) {
            text[tl++] = s[i];
        }
    }
    text[tl] = 0;
    if (depth && nn < 4 && nl) {
        notes[nn][nl] = 0;
        nn++;
    }
    /* the label in capitals throughout ("INF AMMO"): lower case, the
     * spelling decides again */
    int up = 0, letters = 0;
    for (size_t i = 0; text[i]; i++) {
        letters += is_alpha(text[i]);
        up += text[i] >= 'A' && text[i] <= 'Z';
    }
    const bool all_caps = letters >= 4 && up * 10 >= letters * 8;
    Words ws;
    memset(&ws, 0, sizeof(ws));
    read_words(text, &ws);
    char name[256];
    join_words(&ws, all_caps, name, sizeof(name));
    /* the brackets: a player, buttons, no reload, else said as written */
    char said[4][128];
    int ns = 0;
    char btn[128] = "";
    for (int i = 0; i < nn; i++) {
        char lc[96];
        lower_into(lc, notes[i], sizeof(lc));
        while (lc[0] == ' ') {
            memmove(lc, lc + 1, strlen(lc));
        }
        for (size_t l = strlen(lc); l && lc[l - 1] == ' '; l--) {
            lc[l - 1] = 0;
        }
        int pl;
        if (!strcmp(lc, "nr") || !strcmp(lc, "no reload") || !strcmp(lc, "noreload") ||
            !strcmp(lc, "no relaod") || !strcmp(lc, "no reloads")) {
            ws.noreload = true;
        } else if (player_word(lc, &pl)) {
            ws.player = pl;
        } else if (!strncmp(lc, "player ", 7) && lc[7] >= '1' && lc[7] <= '4' && !lc[8]) {
            ws.player = lc[7] - '0';
        } else if (!btn[0] && buttons_text(notes[i], lang, &ws.hotkey, btn, sizeof(btn))) {
            /* the buttons */
        } else if (ns < 4) {
            Words nw;
            memset(&nw, 0, sizeof(nw));
            read_words(notes[i], &nw);
            join_words(&nw, false, said[ns], sizeof(said[ns]));
            ws.player = nw.player ? nw.player : ws.player;
            ws.hotkey |= nw.hotkey;
            ws.noreload |= nw.noreload;
            char key[128];
            lower_into(key, said[ns], sizeof(key));
            if (!strcmp(key, "note") || !strcmp(key, "read note") || !strcmp(key, "see note")) {
                snprintf(key, sizeof(key), "read note");
            }
            char tr[128];
            if (lang != L_EN && tr_core(key, lang - 1, tr, sizeof(tr))) {
                snprintf(said[ns], sizeof(said[ns]), "%s", tr);
            }
            if (said[ns][0]) {
                low_first(said[ns]);
                ns++;
            }
        }
    }
    /* the name, translated when known */
    char key[256];
    lower_into(key, name, sizeof(key));
    char tr[256];
    if (lang != L_EN && name[0] && tr_core(key, lang - 1, tr, sizeof(tr))) {
        snprintf(out, n, "%s", tr);
    } else {
        snprintf(out, n, "%s", name);
        cap_first(out);
    }
    char extra[128];
    if (ws.noreload) {
        if (lang == L_EN || !tr_core("no reload", lang - 1, extra, sizeof(extra))) {
            snprintf(extra, sizeof(extra), "no reload");
        }
        low_first(extra);
        snprintf(said[ns < 4 ? ns++ : 3], sizeof(said[0]), "%s", extra);
    }
    for (int i = 0; i < ns; i++) {
        if (!out[0]) {
            snprintf(out, n, "%s", said[i]);
            cap_first(out);
            continue;
        }
        cat(out, n, " (");
        cat(out, n, said[i]);
        cat(out, n, ")");
    }
    if (ws.player) {
        char p[48];
        snprintf(p, sizeof(p), lang == L_EN ? "player %d" : k_player[lang - 1], ws.player);
        cat(out, n, out[0] ? " (" : "(");
        cat(out, n, p);
        cat(out, n, ")");
    }
    if (btn[0] || ws.hotkey) {
        cat(out, n, out[0] ? " (" : "(");
        cat(out, n, btn[0] ? btn : lang == L_EN ? "hotkey" : k_hotkey[lang - 1]);
        cat(out, n, ")");
    }
}
