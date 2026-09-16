#pragma once

#define IDC_STATIC              (-1)

#define IDI_APPICON             101

#define IDD_SETTINGS            201
#define IDD_PICKAPP             202
#define IDD_PAGE_LAYOUT         210
#define IDD_PAGE_BEHAVIOUR      211
#define IDD_PAGE_SHORTCUTS      212
#define IDD_BINDEDIT            214
#define IDD_PAGE_APPS           215

// ---- shell (main dialog) --------------------------------------------------
#define IDC_APPICON_STATIC      1001
#define IDC_TITLE               1002
#define IDC_STATUS_TEXT         1003
#define IDC_TOGGLE_TILING       1004
#define IDC_TABS                1005
#define IDC_BTN_EDITCFG         1006
#define IDC_BTN_RETILE          1007
#define IDC_APPLY               1008
#define IDC_HIDE                1009

// ---- Layout page ----------------------------------------------------------
#define IDC_LAYOUT              1010
#define IDC_LAYOUT_PREVIEW      1011
#define IDC_LAYOUT_DESC         1012
#define IDC_MASTER_SLIDER       1013
#define IDC_MASTER_VALUE        1014
#define IDC_MASTER_HINT         1015
#define IDC_GAP_INNER           1016
#define IDC_GAP_INNER_SPIN      1017
#define IDC_GAP_OUTER           1018
#define IDC_GAP_OUTER_SPIN      1019
#define IDC_WORKSPACES          1020
#define IDC_WORKSPACES_SPIN     1021
#define IDC_WORKSPACE_HINT      1022

// ---- Behaviour page -------------------------------------------------------
#define IDC_CHK_BORDER          1030
#define IDC_CHK_FFM             1031
#define IDC_CHK_DRAGSWAP        1032
#define IDC_CHK_NEWTOP          1033
#define IDC_CHK_ANIM            1034
#define IDC_ANIM_SPEED          1035
#define IDC_CHK_AUTOSTART       1036
#define IDC_CHK_STARTMIN        1037
#define IDC_EXCLUDE_LIST        1038
#define IDC_BTN_ADD_RUNNING     1039
#define IDC_BTN_ADD_BROWSE      1040
#define IDC_BTN_REMOVE          1041
#define IDC_CHK_GAMEPAUSE       1042
#define IDC_CHK_ELEVAUTO        1043
#define IDC_CHK_MODDRAG         1044

// ---- Shortcuts page -------------------------------------------------------
#define IDC_MODIFIER            1050
#define IDC_BINDLIST            1051
#define IDC_BIND_EDIT           1052
#define IDC_BIND_ADD            1053
#define IDC_BIND_REMOVE         1054
#define IDC_BIND_RESET          1055
#define IDC_CHK_OVERRIDE        1056
#define IDC_BIND_HINT           1057

// ---- Apps page ------------------------------------------------------------
#define IDC_APPLIST             1090
#define IDC_APP_ADD             1091
#define IDC_APP_BROWSE          1092
#define IDC_APP_EDITKEY         1093
#define IDC_APP_REMOVE          1094
#define IDC_APP_HINT            1095

// ---- reserved margins (on the Layout page) --------------------------------
#define IDC_MARGIN_TOP          1061
#define IDC_MARGIN_BOTTOM       1063
#define IDC_MARGIN_LEFT         1065
#define IDC_MARGIN_RIGHT        1067

// ---- Monitor page ---------------------------------------------------------
#define IDD_PAGE_MONITOR        216
#define IDC_MON_ENABLED         1100
#define IDC_MON_PINNED          1101
#define IDC_MON_GRAPHS          1102
#define IDC_MON_VERTICAL        1103
// The eight "what to show" rows. Contiguous and addressed by *slot*, not by
// metric: the page decides which metric each row shows from the saved order,
// so a row's id says where it is on screen and nothing about what is in it.
// The ids they replaced were per-metric and not in metric order, which is
// exactly the trap - IDC_MON_CPU + 3 was Disk.
#define IDC_MON_SHOW_FIRST      1170
#define IDC_MON_SHOW_LAST       1177
#define IDC_MON_OPACITY         1109
#define IDC_MON_OPACITY_VAL     1110
#define IDC_MON_SCALE           1111
#define IDC_MON_SCALE_VAL       1112
#define IDC_MON_INTERVAL        1113
#define IDC_MON_RESET_POS       1114
#define IDC_MON_HINT            1115
#define IDC_MON_THEME           1116
#define IDC_MON_THEME_DESC      1117
#define IDC_MON_PREVIEW         1118
#define IDC_MON_DESKTOP         1119
#define IDC_MON_TOPAPP          1120
#define IDC_MON_STYLE           1121
// Colour swatches, one per metric. Contiguous and in MonMetric order, so the
// handler can turn an id straight into a metric index.
#define IDC_MON_COL_FIRST       1130
#define IDC_MON_COL_CPU         1130
#define IDC_MON_COL_RAM         1131
#define IDC_MON_COL_GPU         1132
#define IDC_MON_COL_VRAM        1133
#define IDC_MON_COL_CPUTEMP     1134
#define IDC_MON_COL_GPUTEMP     1135
#define IDC_MON_COL_DISK        1136
#define IDC_MON_COL_NET         1137
#define IDC_MON_COL_LAST        1137
#define IDC_MON_COL_RESET       1139
// Readout order. The eight rows above are positional - row n shows whichever
// metric the order puts nth - so these move the highlighted one up or down.
#define IDC_MON_ORDER_UP        1140
#define IDC_MON_ORDER_DOWN      1141

// ---- Search page ----------------------------------------------------------
#define IDD_PAGE_SEARCH         217
#define IDC_SRCH_FILES          1150
#define IDC_SRCH_COMMANDS       1151
#define IDC_SRCH_CALC           1152
#define IDC_SRCH_SETTINGS       1153
#define IDC_SRCH_HIDDEN         1154
#define IDC_SRCH_FOLDERS        1155
#define IDC_SRCH_ADD            1156
#define IDC_SRCH_REMOVE         1157
#define IDC_SRCH_DEFAULTS       1158
#define IDC_SRCH_DEPTH          1159
#define IDC_SRCH_MAXENTRIES     1161
#define IDC_SRCH_REINDEX        1162
#define IDC_SRCH_STATUS         1163
#define IDC_SRCH_HINT           1164
// Where programs are looked for, as opposed to files.
#define IDC_SRCH_DRIVES         1165
#define IDC_SRCH_DEEPEXE        1166
#define IDC_SRCH_DRIVELIST      1167

// ---- General page ---------------------------------------------------------
// Setup and maintenance: how it starts, where its settings live, what to do
// when something looks wrong. IDC_CHK_AUTOSTART / STARTMIN / ELEVAUTO moved
// here from the Behaviour page with the Startup group they belong to.
#define IDD_PAGE_GENERAL        218

// Clock page
#define IDD_PAGE_CLOCK          219
#define IDC_CLK_ENABLED         1200
#define IDC_CLK_PINNED          1201
#define IDC_CLK_DESKTOP         1202
#define IDC_CLK_RESET_POS       1203
#define IDC_CLK_STYLE           1204
#define IDC_CLK_STYLE_DESC      1205
#define IDC_CLK_THEME           1206
#define IDC_CLK_THEME_DESC      1207
#define IDC_CLK_24H             1208
#define IDC_CLK_SECONDS         1209
#define IDC_CLK_DATE            1210
#define IDC_CLK_WEEKDAY         1211
#define IDC_CLK_OPACITY         1212
#define IDC_CLK_OPACITY_VAL     1213
#define IDC_CLK_SCALE           1214
#define IDC_CLK_SCALE_VAL       1215
#define IDC_CLK_PREVIEW         1216
#define IDC_CLK_HINT            1217
#define IDC_GEN_OPENCFG         1180
#define IDC_GEN_OPENDIR         1181
#define IDC_GEN_RELOAD          1182
#define IDC_GEN_DIAG            1183
#define IDC_GEN_RESTOREWINS     1184
#define IDC_GEN_RESET           1185
#define IDC_GEN_ABOUT           1186

// ---- "choose an app" picker ----------------------------------------------
#define IDC_PICK_FILTER         1070
#define IDC_PICK_LIST           1071
#define IDC_PICK_PROMPT         1072

// ---- shortcut editor ------------------------------------------------------
#define IDC_BINDEDIT_LABEL      1080
#define IDC_BINDEDIT_COMMAND    1081
#define IDC_BINDEDIT_BROWSE     1082
#define IDC_BINDEDIT_CAPTURE    1083
#define IDC_BINDEDIT_HINT       1084
#define IDC_BINDEDIT_PROGLABEL  1085
