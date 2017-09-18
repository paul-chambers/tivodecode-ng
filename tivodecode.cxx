/*
 * tivodecode-ng
 * Copyright 2006-2015, Jeremy Drake et al.
 * See COPYING file for license terms
 *
 * derived from mpegcat, copyright 2006 Kees Cook, used with permission
 */
#ifdef HAVE_CONFIG_H
#include "tdconfig.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <libgen.h>

#include "getopt_long.h"

#include "cli_common.hxx"
#include "tivo_parse.hxx"
#include "tivo_decoder_ts.hxx"
#include "tivo_decoder_ps.hxx"

int o_no_verify;

static struct option long_options[] = {
    {"mak", 1, 0, 'm'},
    {"out", 1, 0, 'o'},
    {"verbose", 0, 0, 'v'},
    {"pkt-dump", 1, 0, 'p'},
    {"metadata", 0, 0, 'D'},
    {"no-verify", 0, 0, 'n'},
    {"no-video", 0, 0, 'x'},
    {"version", 0, 0, 'V'},
    {"help", 0, 0, 'h'},
    {0, 0, 0, 0}
};

static void do_help(const char *arg0, int exitval)
{
    std::cerr << "Usage: " << arg0 << " [--help] [--verbose|-v] "
        "[--no-verify|-n] [--pkt-dump|-p] pkt_num {--mak|-m} mak "
        "[--metadata|-D] [{--out|-o} outfile] <tivofile>\n\n"
        " -m, --mak         media access key (required)\n"
        " -o, --out,        output file (see notes for default)\n"
        " -v, --verbose,    verbose\n"
        " -p, --pkt-dump,   verbose logging for specific TS packet number\n"
        " -D, --metadata,   dump TiVo recording metadata\n"
        " -n, --no-verify,  do not verify MAK while decoding\n"
        " -x, --no-video,   don't decode video, exit after metadata\n"
        " -V, --version,    print the version information and exit\n"
        " -h, --help,       print this help and exit\n"
        "\n"
        "The file names specified for the output file or the tivo file\n"
        "may be -, which indicates stdout or stdin, respectively.\n"
        "If the output file is not set explicitly, then " << arg0 << " will synthesize\n"
        "one derived from the tivo file and the metadata it contains.\n"
        "\n";
    std::exit(exitval);
}


const unsigned long hashTitle         = 0x0aebc065;
const unsigned long hashSeriesTitle   = 0x7b3fcf9e;
const unsigned long hashEpisodeTitle  = 0x16f37724;
const unsigned long hashEpisodeNumber = 0xf1476d67;
const unsigned long hashShowType      = 0x1eaac51e;
const unsigned long hashMovieYear     = 0x34189552;

char *parseMetadata(char *data)
{
    char *p;
    char *t, tag[32];
    char *v, val[256];
    char prev;
    unsigned long tagHash;
    enum {
        between,
        openingTag,
        closingTag,
        attribute,
        value
    } state = between;
    char   *title = NULL;
    char   *seriesTitle = NULL;
    char   *episodeTitle = NULL;
    int     seasonNumber = -1;
    int     episodeNumber = -1;
    int     movieYear = -1;
    bool    isSeries = true;
    int     n;

    tagHash = 0;
    prev = '\0';
    p = data;
    t = tag;
    v = val;
    while (*p != '\0')
    {
        switch (*p)
        {
        case '<':
            tagHash = 5381; // djb2 hash
            t = tag;
            state = openingTag;
            break;

        case '>':
            switch (state)
            {
            case openingTag:
                if (prev != '/')
                {
                    v = val;
                    state = value;
                }
                else
                    state = between;
                break;

            case closingTag:
                {
                    state = between;
                    *t = '\0';
                    *v = '\0';
                    switch (tagHash & 0xFFFFFFFF) // shouldn't be necessary, but...
                    {
                    case hashSeriesTitle:
                        seriesTitle = strdup(val);
                        break;
                    case hashEpisodeTitle:
                        episodeTitle = strdup(val);
                        break;
                    case hashEpisodeNumber:
                        n = atoi(val);
                        seasonNumber  = n / 100;
                        episodeNumber = n % 100;
                        break;
                    case hashShowType:
                        isSeries = !strcmp(val,"SERIES");
                        break;
                    case hashMovieYear:
                        movieYear = atoi(val);
                        break;
                    case hashTitle:
                        title = strdup(val);
                        break;
                    default:
                        // fprintf(stderr, "tag: \'%s\', tagHash: 0x%x, value: \'%s\'\n", tag, tagHash, val);
                        break;
                    };
                }
                break;
            };
            break;

        case '/':
            if (prev == '<')
                state = closingTag;
            break;

        default:
            if (state == value)
                *v++ = *p;
            else
            {
                *t++ = *p;
                tagHash = ((tagHash << 5) + tagHash) ^ tolower(*p);
            }
            break;
        };
        prev = *p++;
    }

    if (isSeries)
    {
        if (seriesTitle == NULL || episodeTitle == NULL)
            return NULL;

        if (seasonNumber == -1 && episodeNumber == -1)
            std::snprintf(val, sizeof(val), "%s - %s", seriesTitle, episodeTitle);
        else
            std::snprintf(val, sizeof(val), "%s - S%02dE%02d - %s", seriesTitle, seasonNumber, episodeNumber, episodeTitle);
    }
    else
    {
        if (title == NULL)
            return NULL;

        if (movieYear != -1)
            std::snprintf(val, sizeof(val), "%s (%d)", title, movieYear);
        else
            std::snprintf(val, sizeof(val), "%s", title);
    }
    return strdup(val);
}

char *extract(char *data, int size)
{
    char *result,*buf,*strB,*strE;
    int   len;

    if (data == NULL) return NULL;

    buf = (char *)std::malloc(size + 1);
    if (buf == NULL) return NULL;

    std::memcpy(buf,data,size);
    buf[size] = '\0';

    strB = std::strstr(buf,"<showing>");
    if (strB == NULL) return NULL;
    strE = std::strstr(strB,"</showing>");
    if (strE == NULL) return NULL;
    *strE = '\0';

    strB = std::strstr(strB,"<program>");
    if (strB == NULL) return NULL;
    strE = std::strstr(strB,"</program>");
    if (strE == NULL) return NULL;
    *strE = '\0';

    std::fprintf(stderr,"metadata: \'%s\'\n",strB);

    result = parseMetadata(strB);

    std::free(buf);
    return result;
}


int main(int argc, char *argv[])
{
    int o_no_video = 0;
    int o_dump_metadata = 0;
    int makgiven = 0;
    uint32_t pktDump = 0;

    const char *tivofile   = NULL;
          char *destfile   = NULL;
          char *destpath   = NULL;
          char *destbase   = NULL;

    char mak[12];
    std::memset(mak, 0, sizeof(mak));

    TuringState turing;
    std::memset(&turing, 0, sizeof(turing));

    TuringState metaturing;
    std::memset(&metaturing, 0, sizeof(metaturing));
    hoff_t current_meta_stream_pos = 0;

    HappyFile *hfh = NULL, *ofh = NULL;

    TiVoStreamHeader header;
    pktDumpMap.clear();

    while (1)
    {
        int c = getopt_long(argc, argv, "m:o:hnDxvVp:", long_options, 0);

        if (c == -1)
            break;

        switch (c)
        {
            case 'm':
                std::strncpy(mak, optarg, 11);
                mak[11] = '\0';
                makgiven = 1;
                break;
            case 'p':
                std::sscanf(optarg, "%d", &pktDump);
                pktDumpMap[pktDump] = true;
                break;
            case 'o':
                destfile = optarg;
                break;
            case 'h':
                do_help(argv[0], 1);
                break;
            case 'v':
                o_verbose++;
                break;
            case 'n':
                o_no_verify = 1;
                break;
            case 'D' :
                o_dump_metadata = 1;
                break;
            case 'x':
                o_no_video = 1;
                break;
            case '?':
                do_help(argv[0], 2);
                break;
            case 'V':
                do_version(10);
                break;
            default:
                do_help(argv[0], 3);
                break;
        }
    }

    if (!makgiven)
        makgiven = get_mak_from_conf_file(mak);

    if (optind < argc)
    {
        tivofile = argv[optind++];
        if (optind < argc)
            do_help(argv[0], 4);
    }

    if (!makgiven || !tivofile)
    {
        do_help(argv[0], 5);
    }

    char *p = destfile;
    if (p == NULL)
        p = (char *)tivofile;

    p = strdup(p);
    destpath = strdup(dirname(p));
    destbase = strdup(basename(p));

    /* if there's an extension, lop it off */
    char *dot = std::strrchr(destbase,'.');
    if (dot != NULL && std::strlen(dot) < 6)
        *dot = '\0';

    print_qualcomm_msg();

    fprintf(stderr, "reading from %s\n", tivofile);

    hfh = new HappyFile;

    if (!std::strcmp(tivofile, "-"))
    {
        if (!hfh->attach(stdin))
            return 10;
    }
    else
    {
        if (!hfh->open(tivofile, "rb"))
        {
            std::perror(tivofile);
            return 6;
        }
    }

    if (false == header.read(hfh))
    {
        return(8);
    }

    header.dump();

    TiVoStreamChunk *pChunks = new TiVoStreamChunk[header.chunks];
    if (NULL == pChunks)
    {
        std::perror("allocate TiVoStreamChunks");
        return(9);
    }

    for (int32_t i = 0; i < header.chunks; i++)
    {
        hoff_t chunk_start = hfh->tell() + pChunks[i].size();

        if (false == pChunks[i].read(hfh))
        {
            std::perror("chunk read fail");
            return(8);
        }

        switch (pChunks[i].type)
        {
            case TIVO_CHUNK_PLAINTEXT_XML:
                pChunks[i].setupTuringKey(&turing, (uint8_t*)mak);
                pChunks[i].setupMetadataKey(&metaturing, (uint8_t*)mak);
                break;

            case TIVO_CHUNK_ENCRYPTED_XML:
                {
                    uint16_t offsetVal = chunk_start - current_meta_stream_pos;
                    pChunks[i].decryptMetadata(&metaturing, offsetVal);
                    current_meta_stream_pos = chunk_start + pChunks[i].dataSize;
                }
                break;

            default:
                std::perror("Unknown chunk type");
                return(8);
        }

        if (pChunks[i].id == 1)
        {
            char *p;
            p = extract((char *)pChunks[i].pData,pChunks[i].dataSize);
            if (p != NULL)
                destbase = p;
        }

        if (o_dump_metadata)
        {
            char *buf = (char *)malloc(strlen(destpath) + 16);
            std::sprintf(buf, "%s/%s-%02d-%04x.xml", destpath, destbase, i, pChunks[i].id);

            HappyFile *chunkfh = new HappyFile;
            if (!chunkfh->open(buf, "wb"))
            {
                std::perror("create metadata file");
                return 8;
            }

            pChunks[i].dump();

            if (false == pChunks[i].write(chunkfh))
            {
                std::perror("write chunk");
                return 8;
            }

            chunkfh->close();
            delete chunkfh;
        }
    }

//    metaturing.destruct();

    if (o_no_video)
        std::exit(0);

    if ((hfh->tell() > header.mpeg_offset) ||
        (hfh->seek(header.mpeg_offset) < 0))
    {
        std::perror("Error reading header");
        return 8; // I dunno
    }

    ofh = new HappyFile;

    if (destfile == NULL) /* destfile not given on cmdline, so derive one from tivofile and metadata */
    {
        const char *extn;

        switch (header.getFormatType())
        {
        case TIVO_FORMAT_PS:
            extn = "mpg";
            break;
        case TIVO_FORMAT_TS:
            extn = "ts";
            break;
        default:
            extn = "bin";
            break;
        }

        destfile = (char *)std::malloc(strlen(destpath) + strlen(destbase) + strlen(extn) + 3);
        sprintf(destfile, "%s/%s.%s", destpath, destbase, extn);
    }

    fprintf(stderr, "writing to %s\n", destfile);

    if (!std::strcmp(destfile, "-"))
    {
        if (!ofh->attach(stdout))
            return 10;
    }
    else
    {
        if (!ofh->open(destfile, "wb"))
        {
            std::perror("opening output file");
            return 7;
        }
    }

    TiVoDecoder *pDecoder = NULL;

    switch (header.getFormatType())
    {
        case TIVO_FORMAT_PS:
            pDecoder = new TiVoDecoderPS(&turing, hfh, ofh);
            break;

        case TIVO_FORMAT_TS:
            pDecoder = new TiVoDecoderTS(&turing, hfh, ofh);
            break;
    }

    if (NULL == pDecoder)
    {
        std::perror("Unable to create TiVo Decoder");
        return 9;
    }

    if (false == pDecoder->process())
    {
        std::perror("Failed to process file");
        return 9;
    }

    turing.destruct();

    hfh->close();
    delete hfh;

    ofh->close();
    delete ofh;

    return 0;
}

/* vi:set ai ts=4 sw=4 expandtab: */
