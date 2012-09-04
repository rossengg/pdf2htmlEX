/*
 * text.cc
 *
 * Handling text & font, and relative stuffs
 *
 * by WangLu
 * 2012.08.14
 */

#include <iostream>
#include <algorithm>
#include <unordered_set>

#include <boost/format.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>

#include <CharCodeToUnicode.h>
#include <fofi/FoFiTrueType.h>

#include "ff/ff.h"
#include "HTMLRenderer.h"
#include "namespace.h"
#include "config.h"

using boost::algorithm::to_lower;
using std::unordered_set;

path HTMLRenderer::dump_embedded_font (GfxFont * font, long long fn_id)
{
    Object obj, obj1, obj2;
    Object font_obj, font_obj2, fontdesc_obj;
    string suffix;
    path filepath;

    try
    {
        // mupdf consulted
        string subtype;

        auto * id = font->getID();

        Object ref_obj;
        ref_obj.initRef(id->num, id->gen);
        ref_obj.fetch(xref, &font_obj);
        ref_obj.free();

        if(!font_obj.isDict())
        {
            cerr << "Font object is not a dictionary" << endl;
            throw 0;
        }

        Dict * dict = font_obj.getDict();
        if(dict->lookup("DescendantFonts", &font_obj2)->isArray())
        {
            if(font_obj2.arrayGetLength() == 0)
            {
                cerr << "Warning: empty DescendantFonts array" << endl;
            }
            else
            {
                if(font_obj2.arrayGetLength() > 1)
                    cerr << "TODO: multiple entries in DescendantFonts array" << endl;

                if(font_obj2.arrayGet(0, &obj2)->isDict())
                {
                    dict = obj2.getDict();
                }
            }
        }

        if(!dict->lookup("FontDescriptor", &fontdesc_obj)->isDict())
        {
            cerr << "Cannot find FontDescriptor " << endl;
            throw 0;
        }

        dict = fontdesc_obj.getDict();

        if(dict->lookup("FontFile3", &obj)->isStream())
        {
            if(obj.streamGetDict()->lookup("Subtype", &obj1)->isName())
            {
                subtype = obj1.getName();
                if(subtype == "Type1C")
                {
                    suffix = ".cff";
                }
                else if (subtype == "CIDFontType0C")
                {
                    suffix = ".cid";
                }
                else
                {
                    cerr << "Unknown subtype: " << subtype << endl;
                    throw 0;
                }
            }
            else
            {
                cerr << "Invalid subtype in font descriptor" << endl;
                throw 0;
            }
        }
        else if (dict->lookup("FontFile2", &obj)->isStream())
        { 
            suffix = ".ttf";
        }
        else if (dict->lookup("FontFile", &obj)->isStream())
        {
            suffix = ".pfa";
        }
        else
        {
            cerr << "Cannot find FontFile for dump" << endl;
            throw 0;
        }

        if(suffix == "")
        {
            cerr << "Font type unrecognized" << endl;
            throw 0;
        }

        obj.streamReset();

        string fn = (format("f%|1$x|")%fn_id).str();
        ofstream outf;
        filepath = tmp_dir / (fn + suffix);
        outf.open(filepath, ofstream::binary);
        add_tmp_file(fn+suffix);

        char buf[1024];
        int len;
        while((len = obj.streamGetChars(1024, (Guchar*)buf)) > 0)
        {
            outf.write(buf, len);
        }
        outf.close();
        obj.streamClose();
    }
    catch(int) 
    {
        cerr << format("Someting wrong when trying to dump font %|1$x|") % fn_id << endl;
    }

    obj2.free();
    obj1.free();
    obj.free();

    fontdesc_obj.free();
    font_obj2.free();
    font_obj.free();

    return filepath;
}

void HTMLRenderer::embed_font(const path & filepath, GfxFont * font, FontInfo & info, bool get_metric_only)
{
    string suffix = filepath.extension().string();
    to_lower(suffix);

    string fn = (format("f%|1$x|") % info.id).str();

    ff_load_font(filepath.c_str());

    int * code2GID = nullptr;
    int code2GID_len = 0;
    int maxcode = 0;

    Gfx8BitFont * font_8bit = nullptr;

    info.use_tounicode = ((suffix == ".ttf") || (font->isCIDFont()) || (param->always_apply_tounicode));

    if(!get_metric_only)
    {
        /*
         * Step 1
         * dump the font file directly from the font descriptor and put the glyphs into the correct slots
         *
         * for 8bit + nonTrueType
         * re-encoding the font using a PostScript encoding list (glyph id <-> glpyh name)
         *
         * for 8bit + TrueType
         * sort the glpyhs as the original order, and later will map GID (instead of char code) to Unicode
         *
         * for CID + nonTrueType
         * Flatten the font 
         *
         * for CID Truetype
         * same as 8bitTrueType, except for that we have to check 65536 charcodes
         */
        if(!font->isCIDFont())
        {
            font_8bit = dynamic_cast<Gfx8BitFont*>(font);
            maxcode = 0xff;
            if((suffix == ".ttf") || (suffix == ".ttc") || (suffix == ".otf"))
            {
                ff_reencode("original", 0);
                FoFiTrueType *fftt = nullptr;
                if((fftt = FoFiTrueType::load((char*)filepath.c_str())) != nullptr)
                {
                    code2GID = font_8bit->getCodeToGIDMap(fftt);
                    code2GID_len = 256;
                    delete fftt;
                }
            }
            else
            {
                // move the slot such that it's consistent with the encoding seen in PDF
                
                unordered_set<string> nameset;
                bool name_conflict_warned = false;

                memset(cur_mapping2, 0, 256 * sizeof(char*));

                for(int i = 0; i < 256; ++i)
                {
                    auto cn = font_8bit->getCharName(i);
                    if(cn == nullptr)
                    {
                        continue;
                    }
                    else
                    {
                        if(nameset.insert(string(cn)).second)
                        {
                            cur_mapping2[i] = cn;    
                        }
                        else
                        {
                            if(!name_conflict_warned)
                            {
                                name_conflict_warned = true;
                                //TODO: may be resolved using advanced font properties?
                                cerr << "Warning: encoding confliction detected in font: " << fn << endl;
                            }
                        }
                    }
                }

                ff_reencode_raw2(cur_mapping2, 256, 0);
            }
        }
        else
        {
            maxcode = 0xffff;

            if(suffix == ".ttf")
            {
                ff_reencode("original", 0);

                GfxCIDFont * _font = dynamic_cast<GfxCIDFont*>(font);

                // code2GID has been stored for embedded CID fonts
                code2GID = _font->getCIDToGID();
                code2GID_len = _font->getCIDToGIDLen();
            }
            else
            {
                ff_cidflatten();
            }
        }

        /*
         * Step 2
         * map charcode (or GID for CID truetype)
         * generate an Consortium encoding file and let fontforge handle it.
         *
         * - Always map to Unicode for 8bit TrueType fonts and CID fonts
         *
         * - For 8bit nonTruetype fonts:
         *   Try to calculate the correct Unicode value from the glyph names, unless param->always_apply_tounicode is set
         * 
         */


        {
            auto ctu = font->getToUnicode();
            memset(cur_mapping, 0, maxcode * sizeof(int32_t));

            for(int i = 0; i <= maxcode; ++i)
            {
                if((suffix != ".ttf") && (font_8bit != nullptr) && (font_8bit->getCharName(i) == nullptr))
                {
                    continue;
                }

                Unicode u, *pu=&u;
                if(info.use_tounicode)
                {
                    int n = ctu ? (ctu->mapToUnicode(i, &pu)) : 0;
                    u = check_unicode(pu, n, i, font);
                }
                else
                {
                    u = unicode_from_font(i, font);
                }

                cur_mapping[((code2GID && (i < code2GID_len))? code2GID[i] : i)] = u;
            }

            ff_reencode_raw(cur_mapping, maxcode, 1);

            if(ctu)
                ctu->decRefCnt();
        }
    }

    auto dest = ((param->single_html ? tmp_dir : dest_dir) / (fn+(param->font_suffix)));
    if(param->single_html)
        add_tmp_file(fn+(param->font_suffix));

    /*
     * [Win|Typo|HHead][Ascent|Descent]
     * Firefox & Chrome interprets the values in different ways
     * Trying to unify them 
     */
    // Generate an intermediate ttf font in order to retrieve the metrics
    // TODO: see if we can get the values without save/load
    
    string tmp_fn = fn+"_.ttf";
    add_tmp_file(tmp_fn);
    auto tmp_path = tmp_dir / tmp_fn;
    ff_save(tmp_path.c_str());
    ff_close();
    ff_load_font(tmp_path.c_str());

    int em = ff_get_em_size();
    int ascent = ff_get_max_ascent();
    int descent = ff_get_max_descent();

    ff_set_ascent(ascent);
    ff_set_descent(descent);
    ff_save(dest.c_str());
    ff_close();

    if(em != 0)
    {
        info.ascent = ((double)ascent) / em;
        info.descent = -((double)descent) / em;
    }
    else
    {
        info.ascent = 0;
        info.descent = 0;
    }

    // read metric

    if(param->debug)
    {
        cerr << "Ascent: " << info.ascent << " Descent: " << info.descent << endl;
    }
}

void HTMLRenderer::drawString(GfxState * state, GooString * s)
{
    if(s->getLength() == 0)
        return;

    auto font = state->getFont();
    if((font == nullptr) || (font->getWMode()))
    {
        return;
    }

    //hidden
    if((state->getRender() & 3) == 3)
    {
        return;
    }

    // see if the line has to be closed due to state change
    check_state_change(state);
    prepare_line(state);

    // Now ready to output
    // get the unicodes
    char *p = s->getCString();
    int len = s->getLength();

    double dx = 0;
    double dy = 0;
    double dxerr = 0;
    double dx1,dy1;
    double ox, oy;

    int nChars = 0;
    int nSpaces = 0;
    int uLen;

    CharCode code;
    Unicode *u = nullptr;

    while (len > 0) {
        auto n = font->getNextChar(p, len, &code, &u, &uLen, &dx1, &dy1, &ox, &oy);
        
        if(!(_equal(ox, 0) && _equal(oy, 0)))
        {
            cerr << "TODO: non-zero origins" << endl;
        }

        if (n == 1 && *p == ' ') 
        {
            ++nSpaces;
        }
        
        Unicode uu = (cur_font_info.use_tounicode ? check_unicode(u, uLen, code, font) : unicode_from_font(code, font));
        outputUnicodes(line_buf, &uu, 1);

        dx += dx1;
        dy += dy1;

        ++nChars;
        p += n;
        len -= n;
    }

    double hs = state->getHorizScaling();

    // horiz_scaling is merged into ctm now, 
    // so the coordinate system is ugly
    dx = (dx * cur_font_size + nChars * cur_letter_space + nSpaces * cur_word_space) * hs;
    
    dy *= cur_font_size;

    cur_tx += dx;
    cur_ty += dy;
        
    draw_tx += dx + dxerr * cur_font_size * hs;
    draw_ty += dy;
}
