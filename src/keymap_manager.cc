#include "keymap_manager.hh"

namespace std
{

template<> struct hash<Kakoune::KeymapMode>
{
    size_t operator()(Kakoune::KeymapMode val) const
    {
        return hash<int>{}((int)val);
    }
};

}

namespace Kakoune
{

void KeymapManager::map_key(Key key, KeymapMode mode, std::vector<Key> mapping)
{
    m_mapping[{key, mode}] = mapping;
}

void KeymapManager::unmap_key(Key key, KeymapMode mode)
{
    m_mapping.erase({key, mode});
}


bool KeymapManager::is_mapped(Key key, KeymapMode mode) const
{
    return m_mapping.find({key, mode}) != m_mapping.end() or
           (m_parent and m_parent->is_mapped(key, mode));
}

memoryview<Key> KeymapManager::get_mapping(Key key, KeymapMode mode) const
{
    auto it = m_mapping.find({key, mode});
    if (it != m_mapping.end())
        return { it->second };
    kak_assert(m_parent);
    return m_parent->get_mapping(key, mode);
}

}
