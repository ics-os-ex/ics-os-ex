/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison interface for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2021 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

#ifndef YY_YY_LEV_COMP_TAB_H_INCLUDED
# define YY_YY_LEV_COMP_TAB_H_INCLUDED
/* Debug traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif
#if YYDEBUG
extern int yydebug;
#endif

/* Token kinds.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
  enum yytokentype
  {
    YYEMPTY = -2,
    YYEOF = 0,                     /* "end of file"  */
    YYerror = 256,                 /* error  */
    YYUNDEF = 257,                 /* "invalid token"  */
    CHAR = 258,                    /* CHAR  */
    INTEGER = 259,                 /* INTEGER  */
    BOOLEAN = 260,                 /* BOOLEAN  */
    PERCENT = 261,                 /* PERCENT  */
    SPERCENT = 262,                /* SPERCENT  */
    MINUS_INTEGER = 263,           /* MINUS_INTEGER  */
    PLUS_INTEGER = 264,            /* PLUS_INTEGER  */
    MAZE_GRID_ID = 265,            /* MAZE_GRID_ID  */
    SOLID_FILL_ID = 266,           /* SOLID_FILL_ID  */
    MINES_ID = 267,                /* MINES_ID  */
    ROGUELEV_ID = 268,             /* ROGUELEV_ID  */
    MESSAGE_ID = 269,              /* MESSAGE_ID  */
    MAZE_ID = 270,                 /* MAZE_ID  */
    LEVEL_ID = 271,                /* LEVEL_ID  */
    LEV_INIT_ID = 272,             /* LEV_INIT_ID  */
    GEOMETRY_ID = 273,             /* GEOMETRY_ID  */
    NOMAP_ID = 274,                /* NOMAP_ID  */
    OBJECT_ID = 275,               /* OBJECT_ID  */
    COBJECT_ID = 276,              /* COBJECT_ID  */
    MONSTER_ID = 277,              /* MONSTER_ID  */
    TRAP_ID = 278,                 /* TRAP_ID  */
    DOOR_ID = 279,                 /* DOOR_ID  */
    DRAWBRIDGE_ID = 280,           /* DRAWBRIDGE_ID  */
    object_ID = 281,               /* object_ID  */
    monster_ID = 282,              /* monster_ID  */
    terrain_ID = 283,              /* terrain_ID  */
    MAZEWALK_ID = 284,             /* MAZEWALK_ID  */
    WALLIFY_ID = 285,              /* WALLIFY_ID  */
    REGION_ID = 286,               /* REGION_ID  */
    FILLING = 287,                 /* FILLING  */
    IRREGULAR = 288,               /* IRREGULAR  */
    JOINED = 289,                  /* JOINED  */
    ALTAR_ID = 290,                /* ALTAR_ID  */
    LADDER_ID = 291,               /* LADDER_ID  */
    STAIR_ID = 292,                /* STAIR_ID  */
    NON_DIGGABLE_ID = 293,         /* NON_DIGGABLE_ID  */
    NON_PASSWALL_ID = 294,         /* NON_PASSWALL_ID  */
    ROOM_ID = 295,                 /* ROOM_ID  */
    PORTAL_ID = 296,               /* PORTAL_ID  */
    TELEPRT_ID = 297,              /* TELEPRT_ID  */
    BRANCH_ID = 298,               /* BRANCH_ID  */
    LEV = 299,                     /* LEV  */
    MINERALIZE_ID = 300,           /* MINERALIZE_ID  */
    CORRIDOR_ID = 301,             /* CORRIDOR_ID  */
    GOLD_ID = 302,                 /* GOLD_ID  */
    ENGRAVING_ID = 303,            /* ENGRAVING_ID  */
    FOUNTAIN_ID = 304,             /* FOUNTAIN_ID  */
    POOL_ID = 305,                 /* POOL_ID  */
    SINK_ID = 306,                 /* SINK_ID  */
    NONE = 307,                    /* NONE  */
    RAND_CORRIDOR_ID = 308,        /* RAND_CORRIDOR_ID  */
    DOOR_STATE = 309,              /* DOOR_STATE  */
    LIGHT_STATE = 310,             /* LIGHT_STATE  */
    CURSE_TYPE = 311,              /* CURSE_TYPE  */
    ENGRAVING_TYPE = 312,          /* ENGRAVING_TYPE  */
    DIRECTION = 313,               /* DIRECTION  */
    RANDOM_TYPE = 314,             /* RANDOM_TYPE  */
    RANDOM_TYPE_BRACKET = 315,     /* RANDOM_TYPE_BRACKET  */
    A_REGISTER = 316,              /* A_REGISTER  */
    ALIGNMENT = 317,               /* ALIGNMENT  */
    LEFT_OR_RIGHT = 318,           /* LEFT_OR_RIGHT  */
    CENTER = 319,                  /* CENTER  */
    TOP_OR_BOT = 320,              /* TOP_OR_BOT  */
    ALTAR_TYPE = 321,              /* ALTAR_TYPE  */
    UP_OR_DOWN = 322,              /* UP_OR_DOWN  */
    SUBROOM_ID = 323,              /* SUBROOM_ID  */
    NAME_ID = 324,                 /* NAME_ID  */
    FLAGS_ID = 325,                /* FLAGS_ID  */
    FLAG_TYPE = 326,               /* FLAG_TYPE  */
    MON_ATTITUDE = 327,            /* MON_ATTITUDE  */
    MON_ALERTNESS = 328,           /* MON_ALERTNESS  */
    MON_APPEARANCE = 329,          /* MON_APPEARANCE  */
    ROOMDOOR_ID = 330,             /* ROOMDOOR_ID  */
    IF_ID = 331,                   /* IF_ID  */
    ELSE_ID = 332,                 /* ELSE_ID  */
    TERRAIN_ID = 333,              /* TERRAIN_ID  */
    HORIZ_OR_VERT = 334,           /* HORIZ_OR_VERT  */
    REPLACE_TERRAIN_ID = 335,      /* REPLACE_TERRAIN_ID  */
    EXIT_ID = 336,                 /* EXIT_ID  */
    SHUFFLE_ID = 337,              /* SHUFFLE_ID  */
    QUANTITY_ID = 338,             /* QUANTITY_ID  */
    BURIED_ID = 339,               /* BURIED_ID  */
    LOOP_ID = 340,                 /* LOOP_ID  */
    FOR_ID = 341,                  /* FOR_ID  */
    TO_ID = 342,                   /* TO_ID  */
    SWITCH_ID = 343,               /* SWITCH_ID  */
    CASE_ID = 344,                 /* CASE_ID  */
    BREAK_ID = 345,                /* BREAK_ID  */
    DEFAULT_ID = 346,              /* DEFAULT_ID  */
    ERODED_ID = 347,               /* ERODED_ID  */
    TRAPPED_STATE = 348,           /* TRAPPED_STATE  */
    RECHARGED_ID = 349,            /* RECHARGED_ID  */
    INVIS_ID = 350,                /* INVIS_ID  */
    GREASED_ID = 351,              /* GREASED_ID  */
    FEMALE_ID = 352,               /* FEMALE_ID  */
    CANCELLED_ID = 353,            /* CANCELLED_ID  */
    REVIVED_ID = 354,              /* REVIVED_ID  */
    AVENGE_ID = 355,               /* AVENGE_ID  */
    FLEEING_ID = 356,              /* FLEEING_ID  */
    BLINDED_ID = 357,              /* BLINDED_ID  */
    PARALYZED_ID = 358,            /* PARALYZED_ID  */
    STUNNED_ID = 359,              /* STUNNED_ID  */
    CONFUSED_ID = 360,             /* CONFUSED_ID  */
    SEENTRAPS_ID = 361,            /* SEENTRAPS_ID  */
    ALL_ID = 362,                  /* ALL_ID  */
    MONTYPE_ID = 363,              /* MONTYPE_ID  */
    GRAVE_ID = 364,                /* GRAVE_ID  */
    ERODEPROOF_ID = 365,           /* ERODEPROOF_ID  */
    FUNCTION_ID = 366,             /* FUNCTION_ID  */
    MSG_OUTPUT_TYPE = 367,         /* MSG_OUTPUT_TYPE  */
    COMPARE_TYPE = 368,            /* COMPARE_TYPE  */
    UNKNOWN_TYPE = 369,            /* UNKNOWN_TYPE  */
    rect_ID = 370,                 /* rect_ID  */
    fillrect_ID = 371,             /* fillrect_ID  */
    line_ID = 372,                 /* line_ID  */
    randline_ID = 373,             /* randline_ID  */
    grow_ID = 374,                 /* grow_ID  */
    selection_ID = 375,            /* selection_ID  */
    flood_ID = 376,                /* flood_ID  */
    rndcoord_ID = 377,             /* rndcoord_ID  */
    circle_ID = 378,               /* circle_ID  */
    ellipse_ID = 379,              /* ellipse_ID  */
    filter_ID = 380,               /* filter_ID  */
    complement_ID = 381,           /* complement_ID  */
    gradient_ID = 382,             /* gradient_ID  */
    GRADIENT_TYPE = 383,           /* GRADIENT_TYPE  */
    LIMITED = 384,                 /* LIMITED  */
    HUMIDITY_TYPE = 385,           /* HUMIDITY_TYPE  */
    STRING = 386,                  /* STRING  */
    MAP_ID = 387,                  /* MAP_ID  */
    NQSTRING = 388,                /* NQSTRING  */
    VARSTRING = 389,               /* VARSTRING  */
    CFUNC = 390,                   /* CFUNC  */
    CFUNC_INT = 391,               /* CFUNC_INT  */
    CFUNC_STR = 392,               /* CFUNC_STR  */
    CFUNC_COORD = 393,             /* CFUNC_COORD  */
    CFUNC_REGION = 394,            /* CFUNC_REGION  */
    VARSTRING_INT = 395,           /* VARSTRING_INT  */
    VARSTRING_INT_ARRAY = 396,     /* VARSTRING_INT_ARRAY  */
    VARSTRING_STRING = 397,        /* VARSTRING_STRING  */
    VARSTRING_STRING_ARRAY = 398,  /* VARSTRING_STRING_ARRAY  */
    VARSTRING_VAR = 399,           /* VARSTRING_VAR  */
    VARSTRING_VAR_ARRAY = 400,     /* VARSTRING_VAR_ARRAY  */
    VARSTRING_COORD = 401,         /* VARSTRING_COORD  */
    VARSTRING_COORD_ARRAY = 402,   /* VARSTRING_COORD_ARRAY  */
    VARSTRING_REGION = 403,        /* VARSTRING_REGION  */
    VARSTRING_REGION_ARRAY = 404,  /* VARSTRING_REGION_ARRAY  */
    VARSTRING_MAPCHAR = 405,       /* VARSTRING_MAPCHAR  */
    VARSTRING_MAPCHAR_ARRAY = 406, /* VARSTRING_MAPCHAR_ARRAY  */
    VARSTRING_MONST = 407,         /* VARSTRING_MONST  */
    VARSTRING_MONST_ARRAY = 408,   /* VARSTRING_MONST_ARRAY  */
    VARSTRING_OBJ = 409,           /* VARSTRING_OBJ  */
    VARSTRING_OBJ_ARRAY = 410,     /* VARSTRING_OBJ_ARRAY  */
    VARSTRING_SEL = 411,           /* VARSTRING_SEL  */
    VARSTRING_SEL_ARRAY = 412,     /* VARSTRING_SEL_ARRAY  */
    METHOD_INT = 413,              /* METHOD_INT  */
    METHOD_INT_ARRAY = 414,        /* METHOD_INT_ARRAY  */
    METHOD_STRING = 415,           /* METHOD_STRING  */
    METHOD_STRING_ARRAY = 416,     /* METHOD_STRING_ARRAY  */
    METHOD_VAR = 417,              /* METHOD_VAR  */
    METHOD_VAR_ARRAY = 418,        /* METHOD_VAR_ARRAY  */
    METHOD_COORD = 419,            /* METHOD_COORD  */
    METHOD_COORD_ARRAY = 420,      /* METHOD_COORD_ARRAY  */
    METHOD_REGION = 421,           /* METHOD_REGION  */
    METHOD_REGION_ARRAY = 422,     /* METHOD_REGION_ARRAY  */
    METHOD_MAPCHAR = 423,          /* METHOD_MAPCHAR  */
    METHOD_MAPCHAR_ARRAY = 424,    /* METHOD_MAPCHAR_ARRAY  */
    METHOD_MONST = 425,            /* METHOD_MONST  */
    METHOD_MONST_ARRAY = 426,      /* METHOD_MONST_ARRAY  */
    METHOD_OBJ = 427,              /* METHOD_OBJ  */
    METHOD_OBJ_ARRAY = 428,        /* METHOD_OBJ_ARRAY  */
    METHOD_SEL = 429,              /* METHOD_SEL  */
    METHOD_SEL_ARRAY = 430,        /* METHOD_SEL_ARRAY  */
    DICE = 431                     /* DICE  */
  };
  typedef enum yytokentype yytoken_kind_t;
#endif

/* Value type.  */
#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
union YYSTYPE
{
#line 151 "lev_comp.y"

    long    i;
    char    *map;
    struct {
        long room;
        long wall;
        long door;
    } corpos;
    struct {
        long area;
        long x1;
        long y1;
        long x2;
        long y2;
    } lregn;
    struct {
        long x;
        long y;
    } crd;
    struct {
        long ter;
        long lit;
    } terr;
    struct {
        long height;
        long width;
    } sze;
    struct {
        long die;
        long num;
    } dice;
    struct {
        long cfunc;
        char *varstr;
    } meth;

#line 277 "lev_comp.tab.h"

};
typedef union YYSTYPE YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define YYSTYPE_IS_DECLARED 1
#endif


extern YYSTYPE yylval;


int yyparse (void);


#endif /* !YY_YY_LEV_COMP_TAB_H_INCLUDED  */
