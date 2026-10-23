/* The number of the release this source is: 0.1.29 is 129 (minor * 100 + patch; a 0.2.0 would be 200). Raised by
   hand in the commit a release is tagged on. The matchmaking service is told it (gs/net_match.c) and can refuse
   releases older than the one its keeper names (port/matchmaking: the variable MIN_RELEASE). */
#ifndef PORT_RELEASE_H
#define PORT_RELEASE_H
#define PORT_RELEASE 201
/* Where releases are published: the game asks GitHub for the newest one's number once per start (gs/net_match.c,
   "Check for updates"), and its "Update now" (F1) has the setup put it over this folder (port/setup/native.cpp). A
   fork that publishes its own releases names its own repository here. */
#define PORT_UPDATE_REPO "Msaturno64/DecompDB" /* this copy's (the tag team / BT4 mod), not the original's */
#endif
