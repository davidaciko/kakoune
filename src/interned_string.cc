#include "interned_string.hh"

namespace Kakoune
{

InternedString StringRegistry::acquire(StringView str)
{
    auto it = m_slot_map.find(str);
    if (it == m_slot_map.end())
    {
        size_t slot;
        if (not m_free_slots.empty())
        {
            slot = m_free_slots.back();
            m_free_slots.pop_back();
            m_storage[slot] = DataAndRefCount({str.begin(), str.end()}, 1);
        }
        else
        {
            slot = m_storage.size();
            m_storage.push_back(DataAndRefCount({str.begin(), str.end()}, 1));
        }
        // Create a new string view that point to the storage data
        StringView storage_view{m_storage[slot].first.data(), (int)m_storage[slot].first.size()};
        m_slot_map[storage_view] = slot;

        return InternedString{storage_view, InternedString::AlreadyAcquired{}};
    }

    size_t slot = it->second;
    m_storage[slot].second++;
    StringView storage_view{m_storage[slot].first.data(), (int)m_storage[slot].first.size()};
    return InternedString{storage_view, InternedString::AlreadyAcquired{}};
}

void StringRegistry::release(StringView str)
{
    auto it = m_slot_map.find(str);
    kak_assert(it != m_slot_map.end());

    size_t slot = it->second;
    if (--m_storage[slot].second == 0)
    {
        m_free_slots.push_back(slot);
        m_slot_map.erase(it);
        m_storage[slot].first.clear();
    }
}

}
