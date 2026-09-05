// SPDX-License-Identifier: GPL-2.0
/*
 * Mode decision for the MStar front end
 *
 * Copyright (c) 2026 Doug Brown <doug@schmorgal.com>
 */
#include <linux/kernel.h>	/* ARRAY_SIZE; linux/array_size.h is 6.7+ */
#include <linux/limits.h>
#include <linux/string.h>

#include "hd60s-modes.h"

static const struct hd60s_mode hd60s_modes[] = {
	/* hact  vact  htot  vtot   pixclk  Hz il  vic  src */
	{   640,   350,   800,   449,   35483,  70, 0,   0, HD60S_MT_VESA },
	{   640,   350,   800,   368,   25024,  85, 0,   0, HD60S_MT_VESA },
	{   640,   350,   800,   369,   25092,  85, 0,   0, HD60S_MT_VESA },
	{   640,   350,   800,   371,   25228,  85, 0,   0, HD60S_MT_VESA },
	{   640,   350,   832,   445,   31470,  85, 0,   0, HD60S_MT_VESA },
	{   640,   384,   608,   398,   14519,  60, 0,   0, HD60S_MT_VESA },
	{   640,   384,   768,   398,   18339,  60, 0,   0, HD60S_MT_VESA },
	{   640,   384,   608,   403,   14701,  60, 0,   0, HD60S_MT_VESA },
	{   640,   384,   656,   403,   15862,  60, 0,   0, HD60S_MT_VESA },
	{   640,   384,   800,   403,   19344,  60, 0,   0, HD60S_MT_VESA },
	{   640,   384,   848,   440,   14701,  60, 0,   0, HD60S_MT_VESA },
	{   640,   400,   784,   415,   19521,  60, 0,   0, HD60S_MT_VESA },
	{   640,   400,   800,   415,   19920,  60, 0,   0, HD60S_MT_VESA },
	{   640,   400,   800,   417,   29339,  60, 0,   0, HD60S_MT_VESA },
	{   640,   400,   816,   423,   29339,  84, 0,   0, HD60S_MT_VESA },
	{   640,   400,   800,   417,   28356,  85, 0,   0, HD60S_MT_VESA },
	{   640,   400,   816,   421,   29200,  85, 0,   0, HD60S_MT_VESA },
	{   640,   400,   816,   423,   29339,  85, 0,   0, HD60S_MT_VESA },
	{   640,   480,   800,   525,   25200,  57, 0,   0, HD60S_MT_VESA },
	{   640,   480,   800,   514,   25200,  58, 0,   0, HD60S_MT_VESA },
	{   640,   480,   800,   525,   25200,  58, 0,   0, HD60S_MT_VESA },
	{   640,   480,   800,   494,   23712,  60, 0,   1, HD60S_MT_VESA },
	{   640,   480,   800,   500,   24000,  60, 0,   1, HD60S_MT_VESA },
	{   640,   480,   800,   525,   25175,  60, 0,   1, HD60S_MT_EIA },
	{   640,   480,   800,   503,   29552,  70, 0,   0, HD60S_MT_VESA },
	{   640,   480,   800,   522,   29552,  70, 0,   0, HD60S_MT_VESA },
	{   640,   480,   800,   497,   28627,  72, 0,   0, HD60S_MT_VESA },
	{   640,   480,   816,   501,   29434,  72, 0,   0, HD60S_MT_VESA },
	{   640,   480,   816,   503,   29552,  72, 0,   0, HD60S_MT_VESA },
	{   640,   480,   832,   520,   31150,  72, 0,   0, HD60S_MT_VESA },
	{   640,   480,   800,   498,   29880,  75, 0,   0, HD60S_MT_VESA },
	{   640,   480,   840,   500,   31500,  75, 0,   0, HD60S_MT_VESA },
	{   640,   480,   816,   502,   30722,  75, 0,   0, HD60S_MT_VESA },
	{   640,   480,   816,   504,   30844,  75, 0,   0, HD60S_MT_VESA },
	{   640,   480,   800,   500,   34000,  85, 0,   0, HD60S_MT_VESA },
	{   640,   480,   832,   505,   35713,  85, 0,   0, HD60S_MT_VESA },
	{   640,   480,   816,   507,   35165,  85, 0,   0, HD60S_MT_VESA },
	{   640,   480,   832,   509,   35996,  85, 0,   0, HD60S_MT_VESA },
	{   678,   509,   832,   530,   26712,  60, 0,   0, HD60S_MT_VESA },
	{   680,   480,   856,   503,   30139,  70, 0,   0, HD60S_MT_VESA },
	{   720,   240,   858,   262,   13500,  60, 1,   0, HD60S_MT_EIA },
	{   720,   240,   858,   281,   13500,  60, 1,   0, HD60S_MT_VESA },
	{   720,   288,   864,   312,   13500,  50, 1,   0, HD60S_MT_EIA },
	{   720,   400,   900,   440,   35483,  56, 0,   0, HD60S_MT_VESA },
	{   720,   400,   900,   449,   35483,  69, 0,   0, HD60S_MT_VESA },
	{   720,   400,   900,   449,   35483,  70, 0,   0, HD60S_MT_VESA },
	{   720,   400,   880,   419,   31341,  85, 0,   0, HD60S_MT_VESA },
	{   720,   400,   912,   423,   32791,  85, 0,   0, HD60S_MT_VESA },
	{   720,   400,   936,   446,   35483,  85, 0,   0, HD60S_MT_VESA },
	{   720,   480,   858,   530,   27000,  59, 0,   0, HD60S_MT_VESA },
	{   720,   480,   896,   497,   26718,  60, 0,   3, HD60S_MT_VESA },
	{   720,   480,   880,   499,   26347,  60, 0,   3, HD60S_MT_VESA },
	{   720,   480,   896,   500,   26880,  60, 0,   3, HD60S_MT_VESA },
	{   720,   480,   858,   525,   27000,  60, 0,   3, HD60S_MT_EIA },
	{   720,   576,   896,   593,   26566,  50, 0,  18, HD60S_MT_VESA },
	{   720,   576,   880,   595,   26180,  50, 0,  18, HD60S_MT_VESA },
	{   720,   576,   896,   596,   26700,  50, 0,  18, HD60S_MT_VESA },
	{   720,   576,   864,   625,   27000,  50, 0,  18, HD60S_MT_EIA },
	{   720,   576,   864,   627,   27000,  50, 0,  18, HD60S_MT_VESA },
	{   768,   288,   960,   307,   14736,  50, 0,   0, HD60S_MT_VESA },
	{   768,   288,   922,   312,   14736,  50, 0,   0, HD60S_MT_VESA },
	{   768,   512,   960,   529,   27931,  55, 0,   0, HD60S_MT_VESA },
	{   768,   512,   928,   531,   27102,  55, 0,   0, HD60S_MT_VESA },
	{   768,   512,   960,   532,   28089,  55, 0,   0, HD60S_MT_VESA },
	{   768,   512,  1104,   568,   28089,  55, 0,   0, HD60S_MT_VESA },
	{   800,   500,   960,   518,   37296,  75, 0,   0, HD60S_MT_VESA },
	{   800,   500,  1024,   523,   40166,  75, 0,   0, HD60S_MT_VESA },
	{   800,   500,  1024,   525,   40320,  75, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1060,   628,   34483,  39, 0,   0, HD60S_MT_VESA },
	{   800,   600,   886,   643,   34483,  46, 0,   0, HD60S_MT_VESA },
	{   800,   600,   960,   615,   29520,  50, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1008,   618,   31147,  50, 0,   0, HD60S_MT_VESA },
	{   800,   600,   992,   621,   30801,  50, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1064,   666,   34483,  54, 0,   0, HD60S_MT_VESA },
	{   800,   600,   960,   616,   32524,  55, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1008,   620,   34372,  55, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1008,   622,   34483,  55, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1064,   666,   34483,  55, 0,   0, HD60S_MT_VESA },
	{   800,   600,   960,   616,   33116,  56, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1024,   620,   35553,  56, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1008,   623,   35167,  56, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1024,   625,   35840,  56, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1024,   636,   35840,  56, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1024,   626,   35840,  57, 0,   0, HD60S_MT_VESA },
	{   800,   600,   992,   628,   35840,  58, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1018,   671,   35840,  58, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1056,   628,   35840,  59, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1024,   639,   38338,  59, 0,   0, HD60S_MT_VESA },
	{   800,   600,   960,   618,   35596,  60, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1024,   622,   38215,  60, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1024,   624,   38338,  60, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1048,   626,   39790,  60, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1056,   628,   39790,  60, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1056,   629,   39790,  60, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1056,   630,   39790,  60, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1024,   631,   38338,  60, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1056,   646,   39790,  60, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1056,   675,   39790,  60, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1056,   628,   39790,  66, 0,   0, HD60S_MT_VESA },
	{   800,   600,   960,   621,   42923,  72, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1040,   626,   46874,  72, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1040,   628,   47024,  72, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1040,   666,   49870,  72, 0,   0, HD60S_MT_VESA },
	{   800,   600,   960,   622,   44784,  75, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1056,   625,   49500,  75, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1040,   627,   48906,  75, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1040,   629,   49062,  75, 0,   0, HD60S_MT_VESA },
	{   800,   600,   960,   720,   44784,  75, 0,   0, HD60S_MT_VESA },
	{   800,   600,   960,   625,   51000,  85, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1056,   630,   56548,  85, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1048,   631,   56209,  85, 0,   0, HD60S_MT_VESA },
	{   800,   600,  1056,   633,   56818,  85, 0,   0, HD60S_MT_VESA },
	{   800,   600,   900,   675,   56209, 240, 0,   0, HD60S_MT_VESA },
	{   800,   600,   900,   692,   56209, 240, 0,   0, HD60S_MT_VESA },
	{   832,   624,  1088,   654,   53366,  72, 0,   0, HD60S_MT_VESA },
	{   832,   624,   992,   647,   48136,  75, 0,   0, HD60S_MT_VESA },
	{   832,   624,  1088,   652,   53203,  75, 0,   0, HD60S_MT_VESA },
	{   832,   624,  1088,   654,   53366,  75, 0,   0, HD60S_MT_VESA },
	{   832,   624,  1120,   654,   53366,  75, 0,   0, HD60S_MT_VESA },
	{   848,   480,  1008,   494,   29877,  60, 0,   0, HD60S_MT_VESA },
	{   848,   480,  1056,   497,   31489,  60, 0,   0, HD60S_MT_VESA },
	{   848,   480,  1056,   500,   31680,  60, 0,   0, HD60S_MT_VESA },
	{   848,   480,  1088,   517,   33749,  60, 0,   0, HD60S_MT_VESA },
	{  1024,   600,  1234,   628,   35840,  57, 0,   0, HD60S_MT_VESA },
	{  1024,   600,  1184,   619,   43973,  60, 0,   0, HD60S_MT_VESA },
	{  1024,   600,  1312,   624,   49121,  60, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1184,   787,   46590,  50, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1312,   791,   51889,  50, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1312,   793,   52020,  50, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1344,   806,   63912,  59, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1344,   807,   64995,  59, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1184,   790,   56121,  60, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1344,   795,   64108,  60, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1328,   798,   63584,  60, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1344,   802,   64995,  60, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1344,   806,   64995,  60, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1328,   812,   63584,  60, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1328,   819,   63584,  60, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1344,   806,   64995,  61, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1328,   798,   63584,  64, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1344,   800,   68812,  64, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1184,   794,   65806,  70, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1360,   800,   76160,  70, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1344,   802,   75452,  70, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1184,   806,   74925,  70, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1328,   806,   74925,  70, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1216,   807,   75452,  71, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1184,   796,   70684,  75, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1312,   800,   78720,  75, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1312,   801,   78720,  75, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1360,   802,   81804,  75, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1360,   805,   82110,  75, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1184,   800,   80512,  85, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1376,   807,   94386,  85, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1376,   808,   94503,  85, 0,   0, HD60S_MT_VESA },
	{  1024,   768,  1376,   809,   94620,  85, 0,   0, HD60S_MT_VESA },
	{  1024,  1280,  1376,  1327,   49121,  60, 0,   0, HD60S_MT_VESA },
	{  1064,   600,  1224,   618,   45385,  60, 0,   0, HD60S_MT_VESA },
	{  1064,   600,  1352,   624,   50618,  60, 0,   0, HD60S_MT_VESA },
	{  1152,   864,  1312,   889,   69982,  60, 0,   0, HD60S_MT_VESA },
	{  1152,   864,  1520,   895,   81624,  60, 0,   0, HD60S_MT_VESA },
	{  1152,   864,  1520,   897,   81806,  60, 0,   0, HD60S_MT_VESA },
	{  1152,   864,  1536,   902,   96983,  70, 0,   0, HD60S_MT_VESA },
	{  1152,   864,  1312,   895,   88068,  75, 0,   0, HD60S_MT_VESA },
	{  1152,   864,  1600,   900,  108000,  75, 0,   0, HD60S_MT_VESA },
	{  1152,   864,  1552,   902,  104992,  75, 0,   0, HD60S_MT_VESA },
	{  1152,   864,  1536,   905,  104256,  75, 0,   0, HD60S_MT_VESA },
	{  1152,   900,  1550,   937,  108000,  66, 0,   0, HD60S_MT_VESA },
	{  1152,   900,  1550,   943,  108000,  76, 0,   0, HD60S_MT_VESA },
	{  1200,  1920,  1584,  1956,   92949,  30, 0,   0, HD60S_MT_VESA },
	{  1280,   720,  3300,   750,   59400,  24, 0,  60, HD60S_MT_EIA },
	{  1280,   720,  3960,   750,   74250,  25, 0,  61, HD60S_MT_EIA },
	{  1280,   720,  1536,   733,   33776,  30, 0,  62, HD60S_MT_VESA },
	{  1280,   720,  1440,   734,   31708,  30, 0,  62, HD60S_MT_VESA },
	{  1280,   720,  1600,   736,   35328,  30, 0,  62, HD60S_MT_VESA },
	{  1280,   720,  3300,   750,   74250,  30, 0,  62, HD60S_MT_EIA },
	{  1280,   720,  1440,   737,   53064,  50, 0,  19, HD60S_MT_VESA },
	{  1280,   720,  1632,   741,   60465,  50, 0,  19, HD60S_MT_VESA },
	{  1280,   720,  1632,   744,   60710,  50, 0,  19, HD60S_MT_VESA },
	{  1280,   720,  1980,   750,   74250,  50, 0,  19, HD60S_MT_EIA },
	{  1280,   720,  1650,   750,   74250,  57, 0,   0, HD60S_MT_VESA },
	{  1280,   720,  1650,   750,   74250,  59, 0,   0, HD60S_MT_VESA },
	{  1280,   720,  1440,   741,   64022,  60, 0,   4, HD60S_MT_VESA },
	{  1280,   720,  1664,   746,   74480,  60, 0,   4, HD60S_MT_VESA },
	{  1280,   720,  1664,   748,   74680,  60, 0,   4, HD60S_MT_VESA },
	{  1280,   720,  1650,   750,   74250,  60, 0,   4, HD60S_MT_EIA },
	{  1280,   720,  1650,   756,   74250,  60, 0,   4, HD60S_MT_EIA },
	{  1280,   720,  1440,   746,   80568,  75, 0,   0, HD60S_MT_VESA },
	{  1280,   720,  1696,   752,   95654,  75, 0,   0, HD60S_MT_VESA },
	{  1280,   720,  1696,   755,   96036,  75, 0,   0, HD60S_MT_VESA },
	{  1280,   720,  1440,   750,   91800,  85, 0,   0, HD60S_MT_VESA },
	{  1280,   720,  1712,   756,  110013,  85, 0,   0, HD60S_MT_VESA },
	{  1280,   720,  1712,   759,  110449,  85, 0,   0, HD60S_MT_VESA },
	{  1280,   720,  2200,   750,   96036, 100, 0,   0, HD60S_MT_VESA },
	{  1280,   720,  2152,   766,   96036, 100, 0,   0, HD60S_MT_VESA },
	{  1280,   720,  1440,   763,   96036, 120, 0,   0, HD60S_MT_VESA },
	{  1280,   768,  1440,   787,   56664,  50, 0,   0, HD60S_MT_VESA },
	{  1280,   768,  1648,   791,   65178,  50, 0,   0, HD60S_MT_VESA },
	{  1280,   768,  1648,   793,   65343,  50, 0,   0, HD60S_MT_VESA },
	{  1280,   768,  1440,   790,   68256,  60, 0,   0, HD60S_MT_VESA },
	{  1280,   768,  1664,   798,   79672,  60, 0,   0, HD60S_MT_VESA },
	{  1280,   768,  1440,   796,   85968,  75, 0,   0, HD60S_MT_VESA },
	{  1280,   768,  1696,   805,  102396,  75, 0,   0, HD60S_MT_VESA },
	{  1280,   768,  1440,   800,   97920,  85, 0,   0, HD60S_MT_VESA },
	{  1280,   768,  1712,   809,  117725,  85, 0,   0, HD60S_MT_VESA },
	{  1280,   800,  1440,   823,   71107,  60, 0,   0, HD60S_MT_VESA },
	{  1280,   800,  1680,   828,   83462,  60, 0,   0, HD60S_MT_VESA },
	{  1280,   800,  1680,   831,   83764,  60, 0,   0, HD60S_MT_VESA },
	{  1280,   800,  1440,   829,   89532,  75, 0,   0, HD60S_MT_VESA },
	{  1280,   800,  1712,   835,  107214,  75, 0,   0, HD60S_MT_VESA },
	{  1280,   800,  1696,   838,  106593,  75, 0,   0, HD60S_MT_VESA },
	{  1280,   800,  1440,   833,  101959,  85, 0,   0, HD60S_MT_VESA },
	{  1280,   800,  1728,   840,  123379,  85, 0,   0, HD60S_MT_VESA },
	{  1280,   800,  1712,   843,  122673,  85, 0,   0, HD60S_MT_VESA },
	{  1280,   960,  1440,   983,   70776,  50, 0,   0, HD60S_MT_VESA },
	{  1280,   960,  1680,   988,   82992,  50, 0,   0, HD60S_MT_VESA },
	{  1280,   960,  1680,   991,   83244,  50, 0,   0, HD60S_MT_VESA },
	{  1280,   960,  1440,   988,   85363,  60, 0,   0, HD60S_MT_VESA },
	{  1280,   960,  1712,   994,  102103,  60, 0,   0, HD60S_MT_VESA },
	{  1280,   960,  1696,   996,  101353,  60, 0,   0, HD60S_MT_VESA },
	{  1280,   960,  1544,  1000,  108000,  60, 0,   0, HD60S_MT_VESA },
	{  1280,   960,  1800,  1000,  108000,  60, 0,   0, HD60S_MT_VESA },
	{  1280,   960,  1440,   988,   85363,  61, 0,   0, HD60S_MT_VESA },
	{  1280,   960,  1642,   988,  130248,  75, 0,   0, HD60S_MT_VESA },
	{  1280,   960,  1440,   995,  107460,  75, 0,   0, HD60S_MT_VESA },
	{  1280,   960,  1728,  1002,  129859,  75, 0,   0, HD60S_MT_VESA },
	{  1280,   960,  1728,  1005,  130248,  75, 0,   0, HD60S_MT_VESA },
	{  1280,   960,  1440,  1000,  122400,  85, 0,   0, HD60S_MT_VESA },
	{  1280,   960,  1744,  1008,  149425,  85, 0,   0, HD60S_MT_VESA },
	{  1280,   960,  1728,  1011,  148495,  85, 0,   0, HD60S_MT_VESA },
	{  1280,   980,  1728,  1027,  148495,  75, 0,   0, HD60S_MT_VESA },
	{  1280,   980,  1728,  1030,  148495,  75, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1440,  1049,   75528,  50, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1696,  1054,   89379,  50, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1200,  1057,   88788,  50, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1680,  1057,   88788,  50, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1680,  1200,   88788,  50, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1618,  1056,  107964,  59, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1716,  1050,  107964,  60, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1440,  1054,   91065,  60, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1712,  1060,  108883,  60, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1712,  1063,  109191,  60, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1688,  1065,  108883,  60, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1712,  1065,  109191,  60, 0,   0, HD60S_MT_EIA },
	{  1280,  1024,  1688,  1066,  107964,  60, 0,   0, HD60S_MT_EIA },
	{  1280,  1024,  1688,  1067,  108883,  60, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1688,  1068,  108883,  60, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1716,  1111,  107964,  60, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1686,  1117,  107964,  60, 0,   0, HD60S_MT_EIA },
	{  1280,  1024,  1716,  1200,  107964,  60, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1752,  1066,  133125,  70, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1736,  1091,  133125,  71, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1701,  1063,  133125,  72, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1752,  1063,  133125,  72, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1752,  1066,  133125,  72, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1736,  1070,  133125,  72, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1752,  1085,  133125,  72, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1752,  1088,  133125,  72, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1440,  1061,  114588,  75, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1664,  1066,  134955,  75, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1688,  1066,  134955,  75, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1688,  1067,  134955,  75, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1728,  1069,  138542,  75, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1728,  1072,  138931,  75, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1440,  1066,  130478,  85, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1728,  1072,  157455,  85, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1744,  1075,  159358,  85, 0,   0, HD60S_MT_VESA },
	{  1280,  1024,  1744,  1078,  159802,  85, 0,   0, HD60S_MT_VESA },
	{  1360,   768,  1520,   787,   59812,  50, 0,   0, HD60S_MT_VESA },
	{  1360,   768,  1528,   787,   60126,  50, 0,   0, HD60S_MT_VESA },
	{  1360,   768,  1760,   791,   69608,  50, 0,   0, HD60S_MT_VESA },
	{  1360,   768,  1768,   791,   69924,  50, 0,   0, HD60S_MT_VESA },
	{  1360,   768,  1744,   793,   69149,  50, 0,   0, HD60S_MT_VESA },
	{  1360,   768,  1752,   793,   69466,  50, 0,   0, HD60S_MT_VESA },
	{  1360,   768,  1520,   790,   72048,  60, 0,   0, HD60S_MT_VESA },
	{  1360,   768,  1776,   795,   84715,  60, 0,   0, HD60S_MT_VESA },
	{  1360,   768,  1792,   795,   85478,  60, 0,   0, HD60S_MT_VESA },
	{  1360,   768,  1776,   798,   85034,  60, 0,   0, HD60S_MT_VESA },
	{  1360,   768,  1792,   798,   85801,  60, 0,   0, HD60S_MT_VESA },
	{  1360,   768,  1500,   800,   72000,  60, 0,   0, HD60S_MT_VESA },
	{  1400,  1050,  1832,  1083,  100069,  50, 0,   0, HD60S_MT_VESA },
	{  1400,  1050,  1864,  1089,  143922,  59, 0,   0, HD60S_MT_VESA },
	{  1400,  1050,  1560,  1080,  101088,  60, 0,   0, HD60S_MT_VESA },
	{  1400,  1050,  1880,  1087,  122613,  60, 0,   0, HD60S_MT_VESA },
	{  1400,  1050,  1864,  1089,  121793,  60, 0,   0, HD60S_MT_VESA },
	{  1400,  1050,  1560,  1088,  127296,  75, 0,   0, HD60S_MT_VESA },
	{  1400,  1050,  1896,  1096,  155851,  75, 0,   0, HD60S_MT_VESA },
	{  1400,  1050,  1896,  1099,  156277,  75, 0,   0, HD60S_MT_VESA },
	{  1400,  1064,  1920,  1065,  154275,  72, 0,   0, HD60S_MT_VESA },
	{  1424,  1068,  1920,  1116,  154275,  72, 0,   0, HD60S_MT_VESA },
	{  1440,   240,  1792,   259,   27847,  60, 0,   7, HD60S_MT_VESA },
	{  1440,   240,   858,   262,   13500,  60, 0,   7, HD60S_MT_EIA },
	{  1440,   240,   858,   263,   27847,  60, 0,   7, HD60S_MT_VESA },
	{  1440,   288,  1792,   307,   27507,  50, 0,  22, HD60S_MT_VESA },
	{  1440,   288,   864,   312,   13500,  50, 0,  22, HD60S_MT_EIA },
	{  1440,   288,   864,   314,   27507,  50, 0,  22, HD60S_MT_VESA },
	{  1440,   288,   864,   322,   13500,  50, 0,  22, HD60S_MT_EIA },
	{  1440,   540,  1904,   557,   50086,  50, 0,   0, HD60S_MT_VESA },
	{  1440,   540,  1792,   559,   50086,  50, 0,   0, HD60S_MT_VESA },
	{  1440,   900,  1600,   922,   73760,  50, 0,   0, HD60S_MT_VESA },
	{  1440,   900,  1888,   926,   87414,  50, 0,   0, HD60S_MT_VESA },
	{  1440,   900,  1872,   929,   86954,  50, 0,   0, HD60S_MT_VESA },
	{  1440,   900,  1600,   926,   88896,  60, 0,   0, HD60S_MT_VESA },
	{  1440,   900,  1904,   932,  106471,  60, 0,   0, HD60S_MT_VESA },
	{  1440,   900,  1904,   934,  106700,  60, 0,   0, HD60S_MT_VESA },
	{  1440,   900,  1600,   933,  111960,  75, 0,   0, HD60S_MT_VESA },
	{  1440,   900,  1936,   940,  136488,  75, 0,   0, HD60S_MT_VESA },
	{  1440,   900,  1936,   942,  136778,  75, 0,   0, HD60S_MT_VESA },
	{  1440,   900,  1600,   937,  127432,  85, 0,   0, HD60S_MT_VESA },
	{  1440,   900,  1952,   945,  156794,  85, 0,   0, HD60S_MT_VESA },
	{  1440,   900,  1952,   948,  157292,  85, 0,   0, HD60S_MT_VESA },
	{  1600,   900,  1760,   922,   81136,  50, 0,   0, HD60S_MT_VESA },
	{  1600,   900,  2096,   926,   97044,  50, 0,   0, HD60S_MT_VESA },
	{  1600,   900,  2080,   929,   96616,  50, 0,   0, HD60S_MT_VESA },
	{  1600,   900,  1760,   926,   97785,  60, 0,   0, HD60S_MT_VESA },
	{  1600,   900,  2112,   934,  118356,  60, 0,   0, HD60S_MT_VESA },
	{  1600,  1200,  1760,  1229,  108152,  50, 0,   0, HD60S_MT_VESA },
	{  1600,  1200,  2144,  1235,  132392,  50, 0,   0, HD60S_MT_VESA },
	{  1600,  1200,  2128,  1238,  131723,  50, 0,   0, HD60S_MT_VESA },
	{  1600,  1200,  2160,  1250,  162000,  58, 0,   0, HD60S_MT_VESA },
	{  1600,  1200,  2140,  1244,  159361,  59, 0,   0, HD60S_MT_VESA },
	{  1600,  1200,  2092,  1265,  162000,  59, 0,   0, HD60S_MT_VESA },
	{  1600,  1200,  1760,  1235,  130416,  60, 0,   0, HD60S_MT_VESA },
	{  1600,  1200,  2160,  1242,  160963,  60, 0,   0, HD60S_MT_VESA },
	{  1600,  1200,  2160,  1245,  161352,  60, 0,   0, HD60S_MT_VESA },
	{  1600,  1200,  2160,  1250,  162000,  60, 0,   0, HD60S_MT_VESA },
	{  1600,  1200,  2092,  1265,  162000,  60, 0,   0, HD60S_MT_VESA },
	{  1600,  1200,  2112,  1250,  164076,  70, 0,   0, HD60S_MT_VESA },
	{  1600,  1200,  1760,  1243,  164076,  75, 0,   0, HD60S_MT_VESA },
	{  1680,  1050,  1840,  1080,  119232,  60, 0,   0, HD60S_MT_VESA },
	{  1680,  1050,  2240,  1089,  146361,  60, 0,   0, HD60S_MT_VESA },
	{  1920,   540,  2400,   559,   67080,  50, 1,  20, HD60S_MT_VESA },
	{  1920,   540,  2640,   562,   74250,  50, 1,  20, HD60S_MT_EIA },
	{  1920,   540,  2400,   633,   67080,  50, 1,  20, HD60S_MT_VESA },
	{  1920,   540,  2400,   634,   67080,  50, 1,  20, HD60S_MT_VESA },
	{  1920,   540,  2200,   562,   74250,  60, 1,   5, HD60S_MT_EIA },
	{  1920,   540,  2432,   562,   82007,  60, 1,   5, HD60S_MT_VESA },
	{  1920,   540,  2640,   562,  167376, 100, 1,   0, HD60S_MT_VESA },
	{  1920,   540,  2640,   634,  167376, 100, 1,   0, HD60S_MT_VESA },
	{  1920,   624,  2016,   628,   34483,  28, 0,   0, HD60S_MT_VESA },
	{  1920,   720,  2496,   748,   11202,  60, 0,   0, HD60S_MT_VESA },
	{  1920,  1080,  2080,  1094,   54612,  24, 0,  32, HD60S_MT_VESA },
	{  1920,  1080,  2352,  1095,   61810,  24, 0,  32, HD60S_MT_VESA },
	{  1920,  1080,  2400,  1098,   63244,  24, 0,  32, HD60S_MT_VESA },
	{  1920,  1080,  2750,  1125,   74250,  24, 0,  32, HD60S_MT_EIA },
	{  1920,  1080,  2080,  1094,   56888,  25, 0,  33, HD60S_MT_VESA },
	{  1920,  1080,  2368,  1096,   64883,  25, 0,  33, HD60S_MT_VESA },
	{  1920,  1080,  2400,  1099,   65940,  25, 0,  33, HD60S_MT_VESA },
	{  1920,  1080,  2640,  1125,   74250,  25, 0,  33, HD60S_MT_EIA },
	{  1920,  1080,  2080,  1096,   68390,  30, 0,  34, HD60S_MT_VESA },
	{  1920,  1080,  2432,  1099,   80183,  30, 0,  34, HD60S_MT_VESA },
	{  1920,  1080,  2416,  1102,   79873,  30, 0,  34, HD60S_MT_VESA },
	{  1920,  1080,  2200,  1125,   74250,  30, 0,  34, HD60S_MT_EIA },
	{  1920,  1080,  2080,  1106,  115024,  50, 0,  31, HD60S_MT_VESA },
	{  1920,  1080,  2544,  1112,  141446,  50, 0,  31, HD60S_MT_VESA },
	{  1920,  1080,  2544,  1114,  141700,  50, 0,  31, HD60S_MT_VESA },
	{  1920,  1080,  2640,  1125,  148500,  50, 0,  31, HD60S_MT_EIA },
	{  1920,  1080,  2080,  1111,  138652,  60, 0,  16, HD60S_MT_VESA },
	{  1920,  1080,  2200,  1117,  148500,  60, 0,  16, HD60S_MT_VESA },
	{  1920,  1080,  2576,  1118,  172798,  60, 0,  16, HD60S_MT_VESA },
	{  1920,  1080,  2576,  1120,  173107,  60, 0,  16, HD60S_MT_VESA },
	{  1920,  1080,  2200,  1125,  148500,  60, 0,  16, HD60S_MT_EIA },
	{  1920,  1080,  2200,  1131,  148500,  60, 0,  16, HD60S_MT_EIA },
	{  1920,  1200,  2080,  1217,   75940,  30, 0,   0, HD60S_MT_VESA },
	{  1920,  1200,  2448,  1221,   89670,  30, 0,   0, HD60S_MT_VESA },
	{  1920,  1200,  2448,  1224,   89890,  30, 0,   0, HD60S_MT_VESA },
	{  1920,  1200,  2080,  1229,  127816,  50, 0,   0, HD60S_MT_VESA },
	{  1920,  1200,  2560,  1235,  158080,  50, 0,   0, HD60S_MT_VESA },
	{  1920,  1200,  2560,  1238,  158464,  50, 0,   0, HD60S_MT_VESA },
	{  1920,  1200,  2080,  1235,  154128,  60, 0,   0, HD60S_MT_VESA },
	{  1920,  1200,  2592,  1245,  193622,  60, 0,   0, HD60S_MT_VESA },
	{  2048,  1052,  2320,  1092,  179262,  60, 0,   0, HD60S_MT_VESA },
	{  2048,  1536,  2592,  1558,  193622,  30, 0,   0, HD60S_MT_VESA },
	{  3840,  1024,  4832,  1042,  151048,  30, 0,   0, HD60S_MT_VESA },
	{  3840,  1024,  4000,  1043,  125160,  30, 0,   0, HD60S_MT_VESA },
	{  3840,  1024,  4816,  1045,  150981,  30, 0,   0, HD60S_MT_VESA },
};

/*
 * Tolerances. The measured values are quantized by the chip's counters:
 * htotal comes from a 12-bit TMDS-clock count scaled by the deep-color ratio,
 * hactive is rounded up to even, and fv is 1250000/period, so near 60 Hz one
 * count of the period counter is about 0.3 Hz.  vtotal is exact.
 */
#define HD60S_TOL_HTOTAL	8
#define HD60S_TOL_HACTIVE	2
#define HD60S_TOL_VFREQ_DHZ	15	/* 1.5 Hz */

static int absdiff(int a, int b)
{
	return a > b ? a - b : b - a;
}

const struct hd60s_mode *hd60s_find_mode(u16 hactive, u16 htotal, u16 vtotal,
					 u16 vfreq_dhz, bool interlaced)
{
	const struct hd60s_mode *best = NULL;
	int best_score = INT_MAX;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(hd60s_modes); i++) {
		const struct hd60s_mode *m = &hd60s_modes[i];
		int dh, da, df, rep, score;

		/* vtotal is exact. */
		if (m->vtotal != vtotal)
			continue;

		if (!!m->interlaced != interlaced &&
		    !(!m->interlaced && interlaced &&
		      (m->vactive == 240 || m->vactive == 288)))
			continue;

		da = absdiff(m->hactive, hactive);
		if (da > HD60S_TOL_HACTIVE)
			continue;

		/*
		 * htotal, with pixel repetition allowed for.
		 *
		 * The tables store the SD rows in 13.5 MHz units -- CEA mode 6
		 * appears as htotal 858 with hactive 1440 -- while the chip's
		 * counter runs on the 27 MHz TMDS clock and would report 1716.
		 */
		dh = absdiff(m->htotal, htotal);
		rep = 0;
		if (dh > HD60S_TOL_HTOTAL) {
			dh = absdiff(m->htotal * 2, htotal);
			if (dh > HD60S_TOL_HTOTAL * 2)
				continue;
			rep = 1;
		}

		df = absdiff(m->vfreq * 10, vfreq_dhz);
		if (df > HD60S_TOL_VFREQ_DHZ)
			continue;

		/*
		 * Several rows can describe one timing -- the VESA table has
		 * near-duplicates differing only in sync widths. Take the
		 * closest, and break ties towards EIA: an HDMI source sending
		 * a CEA mode is far likelier than a PC mode that happens to
		 * share a vtotal.
		 */
		score = da * 64 + dh * 8 + df + rep * 2 +
			(m->src == HD60S_MT_EIA ? 0 : 4);
		if (score < best_score) {
			best_score = score;
			best = m;
		}
	}
	return best;
}

/*
 * The chip's mode-detect arithmetic
 *
 * The two reference constants are the whole of the unit system: the
 * horizontal period counter runs at 160 MHz and the vertical at 125 kHz
 * (= 160 MHz / 1280), which is why hfreq comes out in units of 100 Hz and fv
 * in units of 0.1 Hz, and why calc_fv = hfreq * 1000 / vtotal is
 * dimensionally the same quantity as fv.
 */
enum hd60s_detect_result hd60s_mstar_decode(const struct hd60s_mstar_regs *r,
					    struct hd60s_detect *out)
{
	u32 htotal, vtotal, hactive, hper, vper, hfreq, fv, calc;

	memset(out, 0, sizeof(*out));

	if ((r->b0_55 & 0x3c) != 0x3c)
		return HD60S_DET_NO_SYNC;

	htotal  = ((u32)(r->b0_6a & 0x0f) << 8) | r->b0_6b;
	vtotal  = ((u32)(r->b0_5b & 0x07) << 8) | r->b0_5c;
	hactive = ((u32)(r->b2_29 & 0x1f) << 8) | r->b2_28;
	hactive += hactive & 1;			/* round up to even */

	hper = ((u32)(r->b0_57 & 0x3f) << 8) | r->b0_58;
	vper = ((u32)(r->b0_59 & 0x3f) << 8) | r->b0_5a;
	hfreq = hper ? 1600000u / hper : 0;
	fv    = vper ? 1250000u / vper : 0;

	/*
	 * Deep color: htotal was counted in TMDS clocks, which run faster
	 * than the pixel clock by 5/4, 3/2 or 2. Scale it back down.
	 */
	if (r->b1_01 & 0x04) {
		switch (r->b2_47 & 0x0f) {
		case 5:		/* 30-bit */
			htotal = htotal * 4 / 5;
			break;
		case 6:		/* 36-bit */
			htotal = htotal * 2 / 3;
			break;
		case 7:		/* 48-bit */
			htotal = htotal / 2;
			break;
		default:
			break;
		}
	}

	calc = vtotal ? hfreq * 1000u / vtotal : 0;

	out->htotal	= htotal;
	out->vtotal	= vtotal;
	out->hactive	= hactive;
	out->hfreq	= hfreq;
	out->fv		= fv;
	out->calc_fv	= calc;
	out->vper	= vper;
	out->interlaced	= (r->b0_5f >> 3) & 1;

	/* the four stability tests, all required */
	if ((r->b0_5c_again ^ r->b0_5c) & 0xfe)
		return HD60S_DET_UNSTABLE;
	if (vtotal < 150)
		return HD60S_DET_UNSTABLE;
	if (fv < 200 || fv > 1500)
		return HD60S_DET_UNSTABLE;
	if (absdiff(calc, fv) > 5)
		return HD60S_DET_UNSTABLE;

	out->mode = hd60s_find_mode((u16)hactive, (u16)htotal,
				    (u16)vtotal, (u16)fv, out->interlaced);
	if (out->mode)
		return HD60S_DET_OK;

	/*
	 * No row -- but the reading may not be a real timing at all.
	 *
	 * When the receiver fails to engage its deep-color depacker it keeps
	 * its whole pixel-clock domain at the TMDS rate, and then htotal,
	 * hactive and the free-measurement window all read the TMDS ratio too
	 * large while the depth nibble above still says 24-bit.
	 */
	if ((r->b1_01 & 0x04) && (r->b2_47 & 0x0f) == 0 &&
	    htotal % 3 == 0 && hactive % 3 == 0) {
		out->deep_row = hd60s_find_mode((u16)(hactive / 3 * 2),
						(u16)(htotal / 3 * 2),
						(u16)vtotal, (u16)fv,
						out->interlaced);
		if (out->deep_row)
			return HD60S_DET_DEEP_MISLOCK;
	}
	return HD60S_DET_NO_MODE;
}

/*
 * 1000/1001, decided from the RAW vertical-period counter.
 *
 * fv truncates - 60.00 and 59.94 both read 599 - but the counter itself keeps
 * the count they differ by. Threshold it directly: within each band that has a
 * fractional variant, a period above the threshold is the 1000/1001 rate.
 *
 * Bands without a fractional variant (50, 25, and every VESA rate) return
 * false. The band edges are fv in 0.1 Hz.
 */
bool hd60s_vper_fractional(u16 vper, u16 fv)
{
	if (fv >= 1191 && fv <= 1209)		/* 120 / 119.88 Hz */
		return vper > 0x412;
	if (fv >= 591 && fv <= 609)		/*  60 /  59.94 Hz */
		return vper > 0x825;
	if (fv >= 291 && fv <= 309)		/*  30 /  29.97 Hz */
		return vper > 0x104d;
	if (fv >= 236 && fv <= 244)		/*  24 /  23.976 Hz */
		return vper > 0x1460;
	return false;
}

/*
 * Color-space conversion.
 *
 * Three rows of (G, R, B) for Y, Cr and Cb, then the output offsets. Chroma
 * is centered by the 0x2000 offsets; luma has none.
 */
static const s16 hd60s_csc_601[9] = {
	 5870,  2990,  1140,		/* Y  from G, R, B */
	-4280,  5110,  -830,		/* Cr from G, R, B */
	-3390, -1720,  5110,		/* Cb from G, R, B */
};

static const s16 hd60s_csc_709[9] = {
	 7152,  2126,   722,
	-4640,  5110,  -470,
	-3940, -1170,  5110,
};

/*
 * The third "colorimetry": none at all. A YCbCr source needs no conversion, and
 * the identity is what remains once hd60s_csc_matrix() applies the controls --
 * a table here rather than a second code path.
 *
 * The (G, R, B) slots carry (Y, Cr, Cb). The order is not guessable from a
 * black frame, where both chroma channels read 128; it comes from the Windows
 * driver's run-time matrix, whose chroma rows are (cos, sin) and (-sin, cos).
 */
static const s16 hd60s_csc_pass[9] = {
	10000,     0,     0,		/* Y  from Y            */
	    0, 10000,     0,		/* Cr from Cr           */
	    0,     0, 10000,		/* Cb from Cb           */
};

/*
 * Quarter-wave sine, Q12, 65 entries spanning 0-90 degrees. The hue control
 * has 256 steps over the full circle, so a quadrant is 64 steps and the table
 * is indexed directly. It holds every value the control can ask for exactly,
 * leaving <linux/cordic.h> nothing to interpolate.
 */
static const s16 hd60s_sin_q12[65] = {
	    0,   101,   201,   301,   401,   501,   601,   700,
	  799,   897,   995,  1092,  1189,  1285,  1380,  1474,
	 1567,  1660,  1751,  1842,  1931,  2019,  2106,  2191,
	 2276,  2359,  2440,  2520,  2598,  2675,  2751,  2824,
	 2896,  2967,  3035,  3102,  3166,  3229,  3290,  3349,
	 3406,  3461,  3513,  3564,  3612,  3659,  3703,  3745,
	 3784,  3822,  3857,  3889,  3920,  3948,  3973,  3996,
	 4017,  4036,  4052,  4065,  4076,  4085,  4091,  4095,
	 4096,
};

/* sin(n * 360/256 degrees) in Q12, n taken modulo 256 */
static s32 hd60s_sin(int n)
{
	n &= 255;
	switch (n >> 6) {
	case 0:	return  hd60s_sin_q12[n];
	case 1:	return  hd60s_sin_q12[128 - n];
	case 2:	return -hd60s_sin_q12[n - 128];
	default: return -hd60s_sin_q12[256 - n];
	}
}

static s32 hd60s_cos(int n)
{
	return hd60s_sin(n + 64);
}

/*
 * Scale 10000 -> fixed point, exactly as the Windows driver rounds it. num is
 * 4096 for Q12; the color-range conversions fold their gain in here rather than
 * anywhere else, which is what the Windows driver does.
 */
static s16 hd60s_csc_q12(s32 v, s32 num)
{
	return (s16)((v * num + 5000) / 10000);
}

/* Q12 numerators for HD60S_CR_BYPASS, _SHRINK and _EXPAND */
static const s32 hd60s_csc_num[3] = { 4096, 3520, 4752 };

void hd60s_csc_matrix(bool bt709, bool passthrough, u8 color_range,
		      const u8 pic[4], s16 out[HD60S_CSC_VALUES])
{
	const s16 *m = passthrough ? hd60s_csc_pass :
		       bt709	   ? hd60s_csc_709 : hd60s_csc_601;
	s32 num = hd60s_csc_num[color_range < 3 ? color_range : 0];
	s32 cr[3], cb[3], sn, cs, yoff;
	unsigned int i;

	/*
	 * Hue rotates the two chroma rows into each other. Done in the
	 * Windows driver's decimal-10000 domain, before the gains and the Q12
	 * conversion, so that a neutral hue is exactly the identity.
	 */
	sn = hd60s_sin((int)pic[3] - 128);
	cs = hd60s_cos((int)pic[3] - 128);
	for (i = 0; i < 3; i++) {
		cr[i] = ((s32)m[3 + i] * cs + (s32)m[6 + i] * sn) / 4096;
		cb[i] = (-(s32)m[3 + i] * sn + (s32)m[6 + i] * cs) / 4096;
	}

	/* contrast is a gain on the luma row, saturation on the chroma rows */
	for (i = 0; i < 3; i++) {
		out[i]     = hd60s_csc_q12((s32)m[i] * pic[1] / 128, num);
		out[3 + i] = hd60s_csc_q12(cr[i] * pic[2] / 128, num);
		out[6 + i] = hd60s_csc_q12(cb[i] * pic[2] / 128, num);
	}

	/*
	 * 0x2000 centers Cr and Cb: half the 14-bit internal scale, so one
	 * 8-bit level is 64 counts. Brightness goes there; the Y row has no
	 * offset of its own.
	 */
	out[9]  = 0x2000;

	/*
	 * The conversion's constant term; the scale above is only its gain.
	 * Shrink is out = 16 + in x 219/255, Expand is out = (in - 16) x
	 * 255/219, so one needs a +16 pedestal and the other -16 x gain.
	 *
	 * NOT the Windows driver's, whose color range is measurably a bare gain
	 * with zero offset, reaching neither 16..235 nor 0..255. The constant is
	 * what lets revs 1-3 advertise V4L2_FMT_FLAG_CSC_QUANTIZATION.
	 *
	 * Correct only for the pairing hd60s_apply_color_range() guarantees:
	 * Shrink on a full input, Expand on a limited one.
	 */
	switch (color_range) {
	case HD60S_CR_SHRINK:
		yoff = 16 * 64;
		break;
	case HD60S_CR_EXPAND:
		yoff = -(16 * num) / 64;
		break;
	default:
		yoff = 0;
		break;
	}
	out[10] = (s16)(((int)pic[0] - 128) * 64 + yoff);
	out[11] = 0x2000;
}
