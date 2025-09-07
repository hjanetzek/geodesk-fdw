/*
 * Test program to debug parent extraction crash for specific features
 */

#include <iostream>
#include <geodesk/geodesk.h>

using namespace geodesk;

int main(int argc, char* argv[])
{
    try
    {
        // Open the GOL file
        Features features("/home/jeff/work/geodesk/data/bremen.gol");
        
        // Test the problematic feature
        int64_t problematic_id = 259654373;
        
        std::cout << "Testing feature " << problematic_id << std::endl;
        
        // Find the feature
        Features result = features("n" + std::to_string(problematic_id));
        
        int count = 0;
        for (Feature f : result)
        {
            count++;
            std::cout << "Found feature: id=" << f.id() 
                     << ", type=" << (f.isNode() ? "node" : f.isWay() ? "way" : "relation")
                     << std::endl;
            
            // Check tags
            std::cout << "Tags:" << std::endl;
            for (Tag tag : f.tags())
            {
                std::cout << "  " << tag.key() << "=" << tag.value() << std::endl;
            }
            
            // Now try to get parents - this is where it might crash
            std::cout << "Getting parents..." << std::endl;
            
            try 
            {
                Features parents = f.parents();
                std::cout << "Parents retrieved successfully" << std::endl;
                
                int parent_count = 0;
                for (Feature parent : parents)
                {
                    parent_count++;
                    std::cout << "  Parent " << parent_count << ": id=" << parent.id() 
                             << ", type=" << (parent.isWay() ? "way" : "relation") << std::endl;
                    
                    // Safety limit
                    if (parent_count > 10)
                    {
                        std::cout << "  (stopping after 10 parents)" << std::endl;
                        break;
                    }
                }
                
                if (parent_count == 0)
                {
                    std::cout << "  No parents found" << std::endl;
                }
            }
            catch (const std::exception& e)
            {
                std::cerr << "ERROR getting parents: " << e.what() << std::endl;
            }
        }
        
        if (count == 0)
        {
            std::cout << "Feature not found!" << std::endl;
        }
        
        // Test a few more power tower nodes
        std::vector<int64_t> other_ids = {259654332, 259654265, 259654266};
        for (int64_t id : other_ids)
        {
            std::cout << "\n---\nTesting feature " << id << std::endl;
            Features result = features("n" + std::to_string(id));
            
            for (Feature f : result)
            {
                std::cout << "Found feature: id=" << f.id() << std::endl;
                
                try 
                {
                    Features parents = f.parents();
                    int parent_count = 0;
                    for (Feature parent : parents)
                    {
                        parent_count++;
                        if (parent_count > 5) break;
                    }
                    std::cout << "Has " << parent_count << " parents" << std::endl;
                }
                catch (const std::exception& e)
                {
                    std::cerr << "ERROR: " << e.what() << std::endl;
                }
            }
        }
        
        std::cout << "\nTest completed successfully!" << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "FATAL ERROR: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}