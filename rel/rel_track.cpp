#include "rel_track.h"
#include <string>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <utility>
#include <algorithm>
#include <set>

rel_track::rel_track()
  : m_valid(false)
{}

rel_track::rel_track(linput_t *p_input)
 : m_valid(false)
 , m_max_filesize( qlsize(p_input) )
 , m_input_file(p_input)
{
  // Read full header
  if (!this->read_header())
  {
    err_msg("REL: Failed to read the header");
    return;
  }

  // Validate header information
  if (!this->validate_header())
  {
    err_msg("REL: Failed simple header validation");
    return;
  }

  // Read sections
  if (!this->read_sections())
  {
    err_msg("REL: Unable to read all sections");
    return;
  }

  m_valid = true;
}

bool rel_track::read_header()
{
  // Read header data from input
  relhdr base_header;
  qlseek(m_input_file, 0, SEEK_SET);
  if (qlread(m_input_file, &base_header, sizeof(base_header)) != sizeof(base_header))
    return err_msg("REL: header is too short or inaccessible");

  // Convert all members from big endian to little endian
  m_id             = swap32(base_header.info.id);
  m_num_sections   = swap32(base_header.info.num_sections);
  m_section_offset = swap32(base_header.info.section_offset);
  m_version        = swap32(base_header.info.version);

  // This data is currently unhandled
  //m_base_header.info.prev           = swap32(base_header.info.prev);
  //m_base_header.info.next           = swap32(base_header.info.next);
  //m_base_header.info.name_offset    = swap32(base_header.info.name_offset); // ignore
  //m_base_header.info.name_size      = swap32(base_header.info.name_size);   // ignore
  m_rel_offset    = swap32(base_header.rel_offset);

  m_import_offset = swap32(base_header.import_offset);
  m_import_size   = swap32(base_header.import_size);
  
  m_bss_section_ign = base_header.bss_section;
  m_bss_size        = swap32(base_header.bss_size);
  
  m_prolog_prep.m_offset     = swap32(base_header.prolog_offset);
  m_prolog_prep.m_section_id = base_header.prolog_section;

  m_epilog_prep.m_offset     = swap32(base_header.epilog_offset);
  m_epilog_prep.m_section_id = base_header.epilog_section;

  m_unresolved_prep.m_offset      = swap32(base_header.unresolved_offset);
  m_unresolved_prep.m_section_id  = base_header.unresolved_section;

  // TODO: v2 check
  //m_base_header.align               = swap32(base_header.align);
  //m_base_header.bss_align           = swap32(base_header.bss_align);
  
  // TODO: v3 check
  //m_base_header.fix_size            = swap32(base_header.fix_size);

  return true;
}

bool rel_track::read_sections()
{
  // Read each section
  qlseek(m_input_file, m_section_offset, SEEK_SET);
  for (unsigned i = 0; i < m_num_sections; ++i)
  {
    // read an entry
    section_entry entry;
    if (qlread(m_input_file, &entry, sizeof(entry)) != sizeof(entry))
      return err_msg("REL: Failed to read section %u", i);

    // Swap endianness
    entry.file_offset  = swap32(entry.file_offset);
    entry.size    = swap32(entry.size);

    if (entry.file_offset == 0 && entry.size != 0)   // bss
    {
      if ( entry.size != m_bss_size)
        return err_msg("BSS section size does not match (%u predicted vs %u declared)", entry.size, m_bss_size);
    }
    else if (entry.file_offset != 0 && entry.size != 0)  // valid
    {
      // Verify boundary
      if (!verify_section(entry.file_offset, entry.size))
        return err_msg("REL: Section is out of bounds");
    }
    m_sections.emplace_back(entry);
  }
  return true;
}

bool rel_track::validate_header() const
{
  // Check for absurd amount of sections
  if (m_num_sections > 32 || m_num_sections <= 1)
    return err_msg("REL: Unlikely number of sections (%u)", m_num_sections);

  // Check section boundary
  if (!verify_section(m_section_offset, m_num_sections*sizeof(section_entry)) )
    return err_msg("REL: Section has overlapping or out of bounds offset (%u entries)", m_num_sections);

  // Check version
  if (m_version <= 0 || m_version > 3)
    return err_msg("REL: Unknown version (%u)", m_version);

  return true;
}

bool rel_track::verify_section(uint32_t offset, uint32_t size) const
{
  offset = SECTION_OFF(offset);
  return sizeof(relhdr) <= offset && (offset + size) <= m_max_filesize;
}

bool rel_track::is_good() const
{
  return m_valid;
}

/*section_entry const * rel_track::get_section(uint entry_id) const
{
  if (entry_id < m_sections.size())
    return &m_sections[entry_id];

  msg("Attempted to retrieve an invalid section (#%u)", entry_id);
  qexit(1);
  return nullptr;
}*/

ea_t rel_track::section_address(uint8_t section, uint32_t offset) const
{
  auto it = m_segment_address_map.find(section);
  if ( it == m_segment_address_map.end() )
    return BADADDR;
  return it->second + offset;
}

bool rel_track::apply_patches(bool dry_run)
{
  if ( !this->create_sections(dry_run) )
    return err_msg("Creating sections failed");

  if ( !this->apply_relocations(dry_run) )
    return err_msg("Relocations failed");

  // TODO: Create Imports

  // TODO: Assign function names
  if ( !this->apply_names(dry_run) )
    return err_msg("Naming failed");

  return true;
}


bool rel_track::create_sections(bool dry_run)
{
  m_next_seg_offset = START;

  // Create sections
  for (size_t i = 0; i < m_sections.size(); ++i)
  {
    auto & entry = m_sections[i];

    // Skip unused
    if ( entry.file_offset == 0 && entry.size == 0 )
      continue;

    std::string type = (entry.file_offset & SECTION_EXEC) ? CLASS_CODE : CLASS_DATA;
    std::string name = (entry.file_offset & SECTION_EXEC) ? NAME_CODE : NAME_DATA;
    name += std::to_string(static_cast<unsigned long long>(i));

    m_segment_address_map[i] = m_next_seg_offset;   // record the loaded segment address
    uint32_t foffset = SECTION_OFF(entry.file_offset);

    // Create the segment
    if ( foffset != 0 )  // known segment
    {
      //if ( foffset < m_next_seg_offset )
        //return err_msg("Segments are not linear (seg #%u)", i);

      if (!add_segm(1, m_next_seg_offset, m_next_seg_offset + entry.size, name.c_str(), type.c_str()))
        return err_msg("Failed to create segment #%u", i);

      if (!file2base(m_input_file, foffset, m_next_seg_offset, m_next_seg_offset + entry.size, FILEREG_PATCHABLE))
        return err_msg("Failed to pull data from file (segment #%u)", i);
    }
    else  // .bss section
    {
      m_internal_bss_section = i;

      if (!add_segm(1, m_next_seg_offset, m_next_seg_offset + entry.size, NAME_BSS, CLASS_BSS))
        return err_msg("Failed to create BSS segment #%u", i);
    }

    set_segm_addressing(getseg(m_next_seg_offset), 1);
    m_next_seg_offset += entry.size;
  }
  return true;
}
bool rel_track::apply_relocations(bool dry_run)
{
  this->init_resolvers(); // initialize user-names

  // Apply relocations
  if (m_import_offset > 0)
  {
    uint32_t count = m_import_size / sizeof(import_entry);
    uint32_t desired_import_size = 0;
    std::map< std::string, std::map<uint32_t, ea_t> > imports_map;
    std::map< std::string, ea_t > imports_module_starts;
    std::set<ea_t> described;

    for (unsigned i = 0; i < count; ++i)
    {
      qlseek(m_input_file, m_import_offset + i*sizeof(import_entry), SEEK_SET);

      // Get the entry
      import_entry entry;
      if (qlread(m_input_file, &entry, sizeof(entry)) != sizeof(entry))
        return err_msg("REL: Failed to read relocation data %u", i);
      // Endianness
      entry.offset = swap32(entry.offset);
      entry.id = swap32(entry.id);

      // Seek to relocations
      qlseek(m_input_file, entry.offset, SEEK_SET);
      uint32_t current_section = 0;
      uint32_t current_offset = 0;
      uint32_t value = 0, where = 0, orig = 0;

      // Self-relocations
      if ( entry.id == m_id )
      {
        for (;;)
        {
          // Read operation
          rel_entry rel;
          if (qlread(m_input_file, &rel, sizeof(rel)) != (sizeof(rel)))
            return err_msg("REL: Failed to read relocation operation @0x%08X - error code: %d", qltell(m_input_file), get_qerrno());

          // endianness
          rel.addend = swap32(rel.addend);
          rel.offset = swap16(rel.offset);

          // Kill if it's the end
          if (rel.type == R_DOLPHIN_END)
            break;

          current_offset += rel.offset;
          switch (rel.type)
          {
          case R_DOLPHIN_SECTION:
            current_section = rel.section;
            current_offset  = 0;
            break;
          case R_DOLPHIN_NOP:
            break;
          case R_PPC_ADDR32:
            patch_dword( this->section_address(current_section, current_offset), this->section_address(rel.section, rel.addend) );
            break;
          case R_PPC_ADDR16_LO:
            patch_word(this->section_address(current_section, current_offset), this->section_address(rel.section, rel.addend) & 0xFFFF);
            break;
          case R_PPC_ADDR16_HA:
            value = this->section_address(rel.section, rel.addend);
            if ((value & 0x8000) == 0x8000)
              value += 0x00010000;

            patch_word(this->section_address(current_section, current_offset), (value >> 16) & 0xFFFF);
            break;
          case R_PPC_REL24:
            where = this->section_address(current_section, current_offset);
            value = this->section_address(rel.section, rel.addend);
            value -= where;
            orig = static_cast<uint32_t>(get_original_dword(where));
            orig &= 0xFC000003;
            orig |= value & 0x03FFFFFC;
            patch_dword(where, orig);
            break;
          default:
            msg("REL: RELOC TYPE %u UNSUPPORTED\n", rel.type);
          }

        }
      }
      else // EXTERNALS
      {
        // Retrieve the module name
        std::string imp_module_name;
        auto it_modname = m_module_names.find(entry.id);
        if ( it_modname != m_module_names.end() )
          imp_module_name = it_modname->second;
        else if ( entry.id == 0 )
          imp_module_name = BASENAME;
        else
          imp_module_name = std::string("module") + std::to_string(static_cast<unsigned long long>(entry.id));

        // Read all imports to get the desired size
        for (;;)
        {
          // Read operation
          rel_entry rel;
          if ( qlread(m_input_file, &rel, sizeof(rel)) != (sizeof(rel)))
            return err_msg("REL: Failed to read relocation operation @0x%08X, id %u", qltell(m_input_file), entry.id);

          // endianness
          rel.addend = swap32(rel.addend);
          rel.offset = swap16(rel.offset);

          // Kill if it's the end
          if (rel.type == R_DOLPHIN_END)
            break;

          if ( rel.type != R_DOLPHIN_SECTION && rel.type != R_DOLPHIN_NOP )
          {
            // Retrieve target offset for import itself
            ea_t target_offset = m_next_seg_offset + desired_import_size;

            // Also try to get a unique address for the module offset
            uint32_t offs = this->get_external_offset(imp_module_name, rel.addend, rel.section);
            if ( offs == 0 || offs == 1 )
              offs = rel.addend + 0x1000000 * rel.section;

            // If the address doesn't exist, then add it and get the next import location
            if ( imports_map[imp_module_name].insert( std::make_pair(offs, target_offset) ).second )
            {
              imports_module_starts.insert( std::make_pair(imp_module_name, target_offset) );
              desired_import_size += 4;
            }
          }

          m_imports[imp_module_name].emplace_back(rel);
        }
      }
    } // for each module
    
    // Now create the import/externals section
    uint32_t imp_offset = m_next_seg_offset;
    m_segment_address_map[SECTION_IMPORTS] = imp_offset;
    //section_entry import_section = { m_next_section_offset, desired_import_size };
    m_next_seg_offset += desired_import_size;
    
    if (!add_segm(1, imp_offset, imp_offset + desired_import_size, NAME_EXTERN, CLASS_EXTERN))
      return err_msg("Failed to create XTRN segment");
    set_segm_addressing(getseg(imp_offset), 1);
    
    m_import_section = static_cast<uint8_t>(m_sections.size());
    //m_sections.emplace_back(import_section);

    // Add and parse imports
    //ea_t targ_offset = this->section_address(m_import_section);
    for ( auto it = m_imports.begin(); it != m_imports.end(); ++it )
    {
      // Add comment for module
      ea_t target_module_start = imports_module_starts[it->first];
      if ( target_module_start == 0 )
        return err_msg("Failed to locate start of module imports.");
      add_extra_cmt( target_module_start, true, "\nImports from %s\n", it->first.c_str() );

      // Iterate relocation opcodes
      uint32_t current_offset = 0, current_section = 0;
      for ( auto e = it->second.begin(); e != it->second.end(); ++e )
      {
        ea_t targ_offset; // this must be initialized for anything that isn't DOLPHIN_SECTION or DOLPHIN_NOP
        
        // If something is actually going to be done with the target
        if ( e->type != R_DOLPHIN_SECTION && e->type != R_DOLPHIN_NOP )
        {
          // Retrieve the address that was used to map to the target import
          uint32_t offs = this->get_external_offset(it->first, e->addend, e->section);
          if ( offs == 0 || offs == 1 )
            offs = e->addend + 0x1000000 * e->section;

          // Retrieve the target offset for the import
          targ_offset = imports_map[it->first][offs];
          if ( targ_offset == 0 )
            return err_msg("Import was not mapped correctly. %s %08X", it->first.c_str(), e->addend);

          // Name the import
          std::ostringstream ss;
          ss << it->first;

          offs = this->get_external_offset(it->first, e->addend, e->section, true);   // re-obtain offs without the unique address generation
          if ( offs == 0 )
          {
            if ( it->first != BASENAME )
              ss << "_s" << static_cast<unsigned>(e->section) << '_';
            ss << reinterpret_cast<void*>(e->addend);
            if ( described.insert(targ_offset).second )
              add_extra_line(targ_offset, true, "addend: %08X; section: %u;", e->addend, static_cast<unsigned>(e->section));
          }
          else if ( offs == 1 )
          {
            ss << "_s" << static_cast<unsigned>(e->section) << "_bss_" << reinterpret_cast<void*>(e->addend);
            if ( described.insert(targ_offset).second )
              add_extra_line(targ_offset, true, "addend: %08X; section: %u (BSS);", e->addend, static_cast<unsigned>(e->section));
          }
          else
          {
            ss << '_' << reinterpret_cast<void*>(offs);
            if ( described.insert(targ_offset).second )
              add_extra_line(targ_offset, true, "addend: %08X; section: %u; virtual: 0x%08X;", e->addend, static_cast<unsigned>(e->section), offs);
          }
          force_name(targ_offset, ss.str().c_str(), 0);
        }

        current_offset += e->offset;
        switch (e->type)
        {
        case R_DOLPHIN_SECTION:
          current_section = e->section;
          current_offset  = 0;
          break;
        case R_DOLPHIN_NOP:
          break;
        case R_PPC_ADDR32:
        {
          patch_dword( this->section_address(current_section, current_offset), targ_offset );
          put_dword(targ_offset, e->addend);
          break;
        }
        case R_PPC_ADDR16_LO:
        {
          patch_word(this->section_address(current_section, current_offset), targ_offset & 0xFFFF);
          put_dword(targ_offset, e->addend);
          break;
        }
        case R_PPC_ADDR16_HA:
        {
          ea_t value = targ_offset;
          if ((value & 0x8000) == 0x8000)
            value += 0x00010000;

          patch_word(this->section_address(current_section, current_offset), (value >> 16) & 0xFFFF);
          put_dword(targ_offset, e->addend);
          break;
        }
        case R_PPC_REL24:
        {
          ea_t where = this->section_address(current_section, current_offset);
          ea_t value = targ_offset;
          value -= where;
          uint32_t orig = static_cast<uint32_t>(get_original_dword(where));
          orig &= 0xFC000003;
          orig |= value & 0x03FFFFFC;
          patch_dword(where, orig);
          break;
        }
        default:
          msg("REL: XTRN RELOC TYPE %u UNSUPPORTED\n", static_cast<unsigned int>(e->type));
        }
      }
    } // for each import
  }
  return true;
}

bool rel_track::apply_names(bool dry_run)
{
  // Describe the binary header
  add_pgm_cmt("ID: %u", m_id);
  add_pgm_cmt("Version: %u", m_version);
  add_pgm_cmt("%u sections @ %08X:", m_num_sections, m_section_offset);
  for ( unsigned i = 0; i < m_sections.size(); ++i )
  {
    if ( i == m_internal_bss_section )
    {
      add_pgm_cmt("    .bss%u: %u bytes", i, m_sections[i].size);
    }
    else if ( m_sections[i].file_offset != 0 )
    {
      if ( m_sections[i].file_offset & SECTION_EXEC )
        add_pgm_cmt("    .text%u: %u bytes @ %08X", i, m_sections[i].size, SECTION_OFF(m_sections[i].file_offset));
      else
        add_pgm_cmt("    .data%u: %u bytes @ %08X", i, m_sections[i].size, SECTION_OFF(m_sections[i].file_offset));
    }
  }
  add_pgm_cmt("Imports: %u bytes @ %08X", m_import_size, m_import_offset);
  add_pgm_cmt("Relocations @ %08X", m_rel_offset);

  // Obtain addresses
  ea_t epilog_addr = section_address(m_epilog_prep.m_section_id, m_epilog_prep.m_offset);
  ea_t prolog_addr = section_address(m_prolog_prep.m_section_id, m_prolog_prep.m_offset);
  ea_t unresolved_addr = section_address(m_unresolved_prep.m_section_id, m_unresolved_prep.m_offset);

  // Make function exports
  add_entry(epilog_addr, epilog_addr, "_epilog", true);
  add_entry(prolog_addr, prolog_addr, "_prolog", true);
  add_entry(unresolved_addr, unresolved_addr, "_unresolved", true);

  // Make library functions (emphasis)
  set_libitem(epilog_addr);
  set_libitem(prolog_addr);
  set_libitem(unresolved_addr);

  return true;
}

int idaapi enum_modules_cb(char const * file, rel_track * owner)
{
  // Load the file
  linput_t * inp = open_linput(file, false);
  rel_track rel(inp);

  // If the file is good
  if ( rel.is_good() )
  {
    std::string basename(qbasename(file));
    std::string modulename = basename.substr(0, basename.find_last_of('.'));

    if ( rel.m_id == 0 )
      msg("%s id is 0\n", modulename.c_str());
    owner->m_module_names[rel.m_id] = modulename;
    owner->m_external_modules[modulename] = rel;
  }

  // close/cleanup
  close_linput(inp);
  return 0;
}

void rel_track::init_resolvers()
{
  std::string path;
  
  // Retrieve the directory of the current database
  char dir[260] = {};
  if ( !qdirname(dir, sizeof(dir), get_path(PATH_TYPE_IDB)) )
    msg("REL: Unable to get directory of idb file.\n");
  path = dir;

  // Load the module names
  m_module_names.clear();
  enumerate_files(nullptr, 0, path.c_str(), "*.rel", reinterpret_cast<int(idaapi*)(char const*,void*)>(&enum_modules_cb), this);


  /*std::ifstream modid(path + "/module_id.txt");
  while( modid >> id >> name )
    m_module_names[id] = name;*/

  // Load the function names
  // TODO: load map files matching module names
}

uint32_t rel_track::get_external_offset(std::string const &modulename, uint32_t offset, uint8_t section, bool virt) const
{
  auto it = m_external_modules.find(modulename);
  // Check for existence
  if ( it == m_external_modules.end() )
  {
    return 0;
  }

  // Check for section validity
  if ( section >= it->second.m_sections.size() )
  {
    msg("REL: Module %s had invalid section reference %u\n", modulename.c_str(), static_cast<unsigned int>(section));
    return 0;
  }

  uint32_t section_offset = SECTION_OFF(it->second.m_sections[section].file_offset);
  if ( section_offset == 0 )
    return 1;

  uint32_t first_offset = 0;
  for ( unsigned i = 0; i < it->second.m_sections.size() && first_offset == 0; ++i )
    first_offset = SECTION_OFF(it->second.m_sections[i].file_offset);
  
  if ( virt )
  {
    section_offset -= first_offset;
    section_offset += START;
  }

  return section_offset + offset;
}
