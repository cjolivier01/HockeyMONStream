#include "hstream/src/libs/common/PersonRoles.h"

int main() {
  if (!hm::IsPlayer(0, "person")) return 1;
  if (!hm::IsPlayer(1, "goalie")) return 1;
  if (hm::IsPlayer(2, "official")) return 1;
  if (hm::IsPlayer(1, "bicycle")) return 1;
  if (!hm::IsPerson(2, "official")) return 1;
  if (hm::IsPerson(1, "bicycle")) return 1;
  if (!std::string_view(hm::PersonRoleText("person")).empty()) return 1;
  if (std::string_view(hm::PersonRoleText("goalie")) != "GOALIE") return 1;
  if (std::string_view(hm::PersonRoleText("official")) != "REF") return 1;
}
