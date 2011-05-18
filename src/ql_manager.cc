//
// ql_manager.cc
//

#include <cstdio>
#include <iostream>
#include <algorithm>
#include <sys/times.h>
#include <sys/types.h>
#include <cassert>
#include <unistd.h>
#include "redbase.h"
#include "ql.h"
#include "sm.h"
#include "ix.h"
#include "rm.h"
#include "iterator.h"
#include "index_scan.h"
#include "file_scan.h"
#include "nested_loop_join.h"
#include <map>

using namespace std;

namespace {
  bool strlt (char* i,char* j) { return (strcmp(i,j) < 0); }
  bool streq (char* i,char* j) { return (strcmp(i,j) == 0); }
};
//
// Constructor for the QL Manager
//
QL_Manager::QL_Manager(SM_Manager &smm_, IX_Manager &ixm_, RM_Manager &rmm_)
  :rmm(rmm_), ixm(ixm_), smm(smm_)
{
  
}

// Users will call - RC invalid = IsValid(); if(invalid) return invalid; 
RC QL_Manager::IsValid () const
{
  bool ret = true;
  ret = ret && (smm.IsValid() == 0);
  return ret ? 0 : QL_BADOPEN;
}

//
// QL_Manager::~QL_Manager()
//
// Destructor for the QL Manager
//
QL_Manager::~QL_Manager()
{
}

//
// Handle the select clause
//
RC QL_Manager::Select(int nSelAttrs, const RelAttr selAttrs_[],
                      int nRelations, const char * const relations_[],
                      int nConditions, const Condition conditions_[])
{
  RC invalid = IsValid(); if(invalid) return invalid;
  int i;

  // copies for rewrite
  RelAttr* selAttrs = new RelAttr[nSelAttrs];
  for (i = 0; i < nSelAttrs; i++) {
    selAttrs[i] = selAttrs_[i];
  }
  

  char** relations = new char*[nRelations];
  for (i = 0; i < nRelations; i++) {
    // strncpy(relations[i], relations_[i], MAXNAME);
    relations[i] = strdup(relations_[i]);
  }

  Condition* conditions = new Condition[nConditions];
  for (i = 0; i < nConditions; i++) {
    conditions[i] = conditions_[i];
  }


  for (i = 0; i < nRelations; i++) {
    RC rc = smm.SemCheck(relations[i]);
    if (rc != 0) return rc;
  }


  sort(relations, 
       relations + nRelations,
       strlt);

  char** dup = adjacent_find(relations,
                             relations + nRelations,
                             streq);
  if(dup != (relations + nRelations))
    return QL_DUPREL;

  // rewrite select *
  bool SELECTSTAR = false;
  if(nSelAttrs == 1 && strcmp(selAttrs[0].attrName, "*") == 0) {
    SELECTSTAR = true;
    nSelAttrs = 0;
    for (int i = 0; i < nRelations; i++) {
      int ac;
      DataAttrInfo * aa;
      RC rc = smm.GetFromTable(relations[i], ac, aa);
      if (rc != 0) return rc;
      nSelAttrs += ac;
      delete aa;
    }
    
    selAttrs = new RelAttr[nSelAttrs];
    int j = 0;
    for (int i = 0; i < nRelations; i++) {
      int ac;
      DataAttrInfo * aa;
      RC rc = smm.GetFromTable(relations[i], ac, aa);
      if (rc != 0) return rc;
      for (int k = 0; k < ac; k++) {
        selAttrs[j].attrName = strdup(aa[k].attrName);
        selAttrs[j].relName = relations[i];
        j++;
      }
      delete aa;
    }
  } // if rewrite select "*"

  for (i = 0; i < nSelAttrs; i++) {
    if(selAttrs[i].relName == NULL) {
      RC rc = smm.FindRelForAttr(selAttrs[i], nRelations, relations);
      if (rc != 0) return rc;
    }
    RC rc = smm.SemCheck(selAttrs[i]);
    if (rc != 0) return rc;
  }
  
  for (i = 0; i < nConditions; i++) {
    if(conditions[i].lhsAttr.relName == NULL) {
      RC rc = smm.FindRelForAttr(conditions[i].lhsAttr, nRelations, relations);
      if (rc != 0) return rc;
    }
    RC rc = smm.SemCheck(conditions[i].lhsAttr);
    if (rc != 0) return rc;
    
    if(conditions[i].bRhsIsAttr == TRUE) {
      if(conditions[i].rhsAttr.relName == NULL) {
        RC rc = smm.FindRelForAttr(conditions[i].rhsAttr, nRelations, relations);
        if (rc != 0) return rc;
      }
      RC rc = smm.SemCheck(conditions[i].rhsAttr);
      if (rc != 0) return rc;
    }

    rc = smm.SemCheck(conditions[i]);
    if (rc != 0) return rc;
  }


  cout << "Select\n";

  cout << "   nSelAttrs = " << nSelAttrs << "\n";
  for (i = 0; i < nSelAttrs; i++)
    cout << "   selAttrs[" << i << "]:" << selAttrs[i] << "\n";

  cout << "   nRelations = " << nRelations << "\n";
  for (i = 0; i < nRelations; i++)
    cout << "   relations[" << i << "] " << relations[i] << "\n";

  cout << "   nCondtions = " << nConditions << "\n";
  for (i = 0; i < nConditions; i++)
    cout << "   conditions[" << i << "]:" << conditions[i] << "\n";



  if(SELECTSTAR)
    for(int i = 0; i < nSelAttrs; i++)
      free(selAttrs[i].attrName);
  delete [] selAttrs;
  for (i = 0; i < nRelations; i++) {
    free(relations[i]);
  }
  delete [] relations;
  delete [] conditions;
  return 0;
}

//
// Insert the values into relName
//
RC QL_Manager::Insert(const char *relName,
                      int nValues, const Value values[])
{
  RC invalid = IsValid(); if(invalid) return invalid;

  RC rc = smm.SemCheck(relName);
  if (rc != 0) return rc;

  int attrCount;
  DataAttrInfo* attr;
  rc = smm.GetFromTable(relName, attrCount, attr);

  if(nValues != attrCount) {
    delete [] attr;
    return QL_INVALIDSIZE;
  }

  int size = 0;
  for(int i =0; i < nValues; i++) {
    if(values[i].type != attr[i].attrType) {
      delete [] attr;
      return QL_JOINKEYTYPEMISMATCH;
    }
    size += attr[i].attrLength;
  }

  char * buf = new char[size];
  int offset = 0;
  for(int i =0; i < nValues; i++) {
    assert(values[i].data != NULL);
    memcpy(buf + offset,
           values[i].data,
           attr[i].attrLength);
    offset += attr[i].attrLength;
  }

  rc = smm.LoadRecord(relName, size, buf);
  if (rc != 0) return rc;

  Printer p(attr, attrCount);
  p.PrintHeader(cout);
  p.Print(cout, buf);
  p.PrintFooter(cout);

  delete [] attr;
  delete [] buf;
  int i;

  cout << "Insert\n";

  cout << "   relName = " << relName << "\n";
  cout << "   nValues = " << nValues << "\n";
  for (i = 0; i < nValues; i++)
    cout << "   values[" << i << "]:" << values[i] << "\n";

  return 0;
}

//
// Delete from the relName all tuples that satisfy conditions
//
RC QL_Manager::Delete(const char *relName_,
                      int nConditions, const Condition conditions_[])
{
  RC invalid = IsValid(); if(invalid) return invalid;

  char relName[MAXNAME];
  strncpy(relName, relName_, MAXNAME);

  RC rc = smm.SemCheck(relName);
  if (rc != 0) return rc;

  Condition* conditions = new Condition[nConditions];
  for (int i = 0; i < nConditions; i++) {
    conditions[i] = conditions_[i];
  }

  for (int i = 0; i < nConditions; i++) {
    if(conditions[i].lhsAttr.relName == NULL) {
      conditions[i].lhsAttr.relName = relName;
    }
    if(strcmp(conditions[i].lhsAttr.relName, relName) != 0) {
      delete [] conditions;
      return QL_BADATTR;
    }

    RC rc = smm.SemCheck(conditions[i].lhsAttr);
    if (rc != 0) return rc;
    
    if(conditions[i].bRhsIsAttr == TRUE) {
      if(conditions[i].rhsAttr.relName == NULL) {
        conditions[i].rhsAttr.relName = relName;
      }
      if(strcmp(conditions[i].rhsAttr.relName, relName) != 0) {
        delete [] conditions;
        return QL_BADATTR;
      }

      RC rc = smm.SemCheck(conditions[i].rhsAttr);
      if (rc != 0) return rc;
    }

    rc = smm.SemCheck(conditions[i]);
    if (rc != 0) return rc;
  }

  Iterator* it = GetLeafIterator(relName, nConditions, conditions);

  if(bQueryPlans == TRUE)
    cout << "\n" << it->Explain() << "\n";

  Tuple t = it->GetTuple();
  rc = it->Open();
  if (rc != 0) return rc;

  Printer p(t);
  p.PrintHeader(cout);

  RM_FileHandle fh;
  rc =	rmm.OpenFile(relName, fh);
  if (rc != 0) return rc;

  int attrCount = -1;
  DataAttrInfo * attributes;
  rc = smm.GetFromTable(relName, attrCount, attributes);
  if(rc != 0) return rc;
  IX_IndexHandle * indexes = new IX_IndexHandle[attrCount];
  for (int i = 0; i < attrCount; i++) {
    if(attributes[i].indexNo != -1) {
      ixm.OpenIndex(relName, attributes[i].indexNo, indexes[i]);
    }
  }

  while(1) {
    rc = it->GetNext(t);
    if(rc ==  it->Eof())
      break;
    if (rc != 0) return rc;

    rc = fh.DeleteRec(t.GetRid());
    if (rc != 0) return rc;

    for (int i = 0; i < attrCount; i++) {
      if(attributes[i].indexNo != -1) {
        void * pKey;
        t.Get(attributes[i].offset, pKey);
        indexes[i].DeleteEntry(pKey, t.GetRid());
      }
    }

    p.Print(cout, t);
  }

  p.PrintFooter(cout);
  rc = it->Close();
  if (rc != 0) return rc;

  for (int i = 0; i < attrCount; i++) {
    if(attributes[i].indexNo != -1) {
      RC rc = ixm.CloseIndex(indexes[i]);
      if(rc != 0 ) return rc;
    }
  }
  delete [] indexes;

  rc =	rmm.CloseFile(fh);
  if (rc != 0) return rc;
 
  int i;

  cout << "Delete\n";

  cout << "   relName = " << relName << "\n";
  cout << "   nCondtions = " << nConditions << "\n";
  for (i = 0; i < nConditions; i++)
    cout << "   conditions[" << i << "]:" << conditions[i] << "\n";

  delete [] conditions;
  return 0;
}


//
// Update from the relName all tuples that satisfy conditions
//
RC QL_Manager::Update(const char *relName_,
                      const RelAttr &updAttr_,
                      const int bIsValue,
                      const RelAttr &rhsRelAttr,
                      const Value &rhsValue,
                      int nConditions, const Condition conditions_[])
{
  RC invalid = IsValid(); if(invalid) return invalid;

  char relName[MAXNAME];
  strncpy(relName, relName_, MAXNAME);

  RC rc = smm.SemCheck(relName);
  if (rc != 0) return rc;

  Condition* conditions = new Condition[nConditions];
  for (int i = 0; i < nConditions; i++) {
    conditions[i] = conditions_[i];
  }

  RelAttr updAttr;
  updAttr.relName = relName;
  updAttr.attrName = updAttr_.attrName;
  rc = smm.SemCheck(updAttr);
  if (rc != 0) return rc;

  Condition cond;
  cond.lhsAttr = updAttr;
  cond.bRhsIsAttr = (bIsValue == TRUE ? FALSE : TRUE);
  cond.rhsAttr.attrName = rhsRelAttr.attrName;
  cond.rhsAttr.relName = relName;
  cond.op = EQ_OP;
  cond.rhsValue.type = rhsValue.type;
  cond.rhsValue.data = rhsValue.data;

  if (bIsValue != TRUE) {
    updAttr.attrName = rhsRelAttr.attrName;
    rc = smm.SemCheck(updAttr);
    if (rc != 0) return rc;
  }

  rc = smm.SemCheck(cond);
  if (rc != 0) return rc;
  
  for (int i = 0; i < nConditions; i++) {
    if(conditions[i].lhsAttr.relName == NULL) {
      conditions[i].lhsAttr.relName = relName;
    }
    if(strcmp(conditions[i].lhsAttr.relName, relName) != 0) {
      delete [] conditions;
      return QL_BADATTR;
    }

    RC rc = smm.SemCheck(conditions[i].lhsAttr);
    if (rc != 0) return rc;
    
    if(conditions[i].bRhsIsAttr == TRUE) {
      if(conditions[i].rhsAttr.relName == NULL) {
        conditions[i].rhsAttr.relName = relName;
      }
      if(strcmp(conditions[i].rhsAttr.relName, relName) != 0) {
        delete [] conditions;
        return QL_BADATTR;
      }

      RC rc = smm.SemCheck(conditions[i].rhsAttr);
      if (rc != 0) return rc;
    }

    rc = smm.SemCheck(conditions[i]);
    if (rc != 0) return rc;
  }

  Iterator* it;
  // handle halloween problem by not choosing indexscan on an attr when the attr
  // is the one being updated.
  if(smm.IsAttrIndexed(updAttr.relName, updAttr.attrName)) {
    // temporarily make attr unindexed
    rc = smm.DropIndexFromAttrCatAlone(updAttr.relName, updAttr.attrName);
    if (rc != 0) return rc;

    it = GetLeafIterator(relName, nConditions, conditions);

    rc = smm.ResetIndexFromAttrCatAlone(updAttr.relName, updAttr.attrName);
    if (rc != 0) return rc;
  } else {
    it = GetLeafIterator(relName, nConditions, conditions);
  }

  if(bQueryPlans == TRUE)
    cout << "\n" << it->Explain() << "\n";

  Tuple t = it->GetTuple();
  rc = it->Open();
  if (rc != 0) return rc;

  void * val = NULL;
  if(bIsValue == TRUE)
    val = rhsValue.data;
  else
    t.Get(rhsRelAttr.attrName, val);

  Printer p(t);
  p.PrintHeader(cout);

  RM_FileHandle fh;
  rc =	rmm.OpenFile(relName, fh);
  if (rc != 0) return rc;

  int attrCount = -1;
  int updAttrOffset = -1;
  DataAttrInfo * attributes;
  rc = smm.GetFromTable(relName, attrCount, attributes);
  if(rc != 0) return rc;
  IX_IndexHandle * indexes = new IX_IndexHandle[attrCount];
  for (int i = 0; i < attrCount; i++) {
    if(attributes[i].indexNo != -1 && 
       strcmp(attributes[i].attrName, updAttr.attrName) == 0) {
      ixm.OpenIndex(relName, attributes[i].indexNo, indexes[i]);
    }
    if(strcmp(attributes[i].attrName, updAttr.attrName) == 0) {
      updAttrOffset = attributes[i].offset;
    }
  }

  while(1) {
    rc = it->GetNext(t);
    if(rc ==  it->Eof())
      break;
    if (rc != 0) return rc;

    RM_Record rec;
    
    for (int i = 0; i < attrCount; i++) {
    if(attributes[i].indexNo != -1 && 
       strcmp(attributes[i].attrName, updAttr.attrName) == 0) {
        void * pKey;
        t.Get(attributes[i].offset, pKey);
        rc = indexes[i].DeleteEntry(pKey, t.GetRid());
        if (rc != 0) return rc;
        rc = indexes[i].InsertEntry(val, t.GetRid());
        if (rc != 0) return rc;
      }
    }

    t.Set(updAttrOffset, val);
    char * newbuf;
    t.GetData(newbuf);
    rec.Set(newbuf, it->TupleLength(), t.GetRid());
    rc = fh.UpdateRec(rec);
    if (rc != 0) return rc;

    p.Print(cout, t);
  }

  p.PrintFooter(cout);
  rc = it->Close();
  if (rc != 0) return rc;

  for (int i = 0; i < attrCount; i++) {
    if(attributes[i].indexNo != -1 && 
       strcmp(attributes[i].attrName, updAttr.attrName) == 0) {
      RC rc = ixm.CloseIndex(indexes[i]);
      if(rc != 0 ) return rc;
    }
  }
  delete [] indexes;

  rc =	rmm.CloseFile(fh);
  if (rc != 0) return rc;

  int i;

  cout << "Update\n";

  cout << "   relName = " << relName << "\n";
  cout << "   updAttr:" << updAttr << "\n";
  if (bIsValue)
    cout << "   rhs is value: " << rhsValue << "\n";
  else
    cout << "   rhs is attribute: " << rhsRelAttr << "\n";

  cout << "   nCondtions = " << nConditions << "\n";
  for (i = 0; i < nConditions; i++)
    cout << "   conditions[" << i << "]:" << conditions[i] << "\n";

  return 0;
}

//
// Choose between filescan and indexscan for first operation - leaf level of
// operator tree
// REturned iterator should be deleted by user after use.
Iterator* QL_Manager::GetLeafIterator(const char *relName,
                                      int nConditions, const Condition conditions[])
{  
  RC invalid = IsValid(); if(invalid) return NULL;

  if(relName == NULL) {
    return NULL;
  }

  int attrCount = -1;
  DataAttrInfo * attributes;
  RC rc = smm.GetFromTable(relName, attrCount, attributes);
  if(rc != 0) return NULL;

  int nIndexes = 0;
  char* chosenIndex = NULL;
  const Condition * chosenCond = NULL;
  Condition * filters = NULL;
  int nFilters = -1;

  map<string, const Condition*> keys;

  for(int j = 0; j < nConditions; j++) {
    if(strcmp(conditions[j].lhsAttr.relName, relName) == 0) {
      keys[string(conditions[j].lhsAttr.attrName)] = &conditions[j];
    }

    if(conditions[j].bRhsIsAttr == TRUE &&
       strcmp(conditions[j].rhsAttr.relName, relName) == 0) {
      keys[string(conditions[j].rhsAttr.attrName)] = &conditions[j];
    }
  }
  
  for(map<string, const Condition*>::iterator it = keys.begin(); it != keys.end(); it++) {
    // Pick last numerical index or at least one non-numeric index
    for (int i = 0; i < attrCount; i++) {
      if(attributes[i].indexNo != -1 && 
         strcmp(it->first.c_str(), attributes[i].attrName) == 0) {
        nIndexes++;
        if(chosenIndex == NULL ||
           attributes[i].attrType == INT || attributes[i].attrType == FLOAT) {
          chosenIndex = attributes[i].attrName;
          chosenCond = it->second;
        }
      }
    }
  }

  if(chosenCond == NULL) {
    nFilters = nConditions;
    filters = new Condition[nFilters];
    for(int j = 0; j < nConditions; j++) {
      if(chosenCond != &(conditions[j])) {
        filters[j] = conditions[j];
      }
    }
  } else {
    nFilters = nConditions - 1;
    filters = new Condition[nFilters];
    for(int j = 0, k = 0; j < nConditions; j++) {
      if(chosenCond != &(conditions[j])) {
        filters[k] = conditions[j];
        k++;
      }
    }
  }

  if(nConditions == 0 || nIndexes == 0) {
    Condition cond = NULLCONDITION;

    RC status = -1;
    Iterator* it = NULL;
    if(nConditions == 0)
      it = new FileScan(smm, rmm, relName, status, cond);
    else
      it = new FileScan(smm, rmm, relName, status, cond, nConditions,
                        conditions);
    
    if(status != 0) {
      PrintErrorAll(status);
      return NULL;
    }
    delete [] attributes;
    return it;
  }

  // use an index scan
  RC status = -1;
  Iterator* it;

  if(chosenCond != NULL)
    it = new IndexScan(smm, rmm, ixm, relName, chosenIndex, status,
                       *chosenCond, nFilters, filters);
  else
    it = new IndexScan(smm, rmm, ixm, relName, chosenIndex, status,
                       NULLCONDITION, nFilters, filters);

  if(status != 0) {
    PrintErrorAll(status);
    return NULL;
  }

  delete [] filters;
  delete [] attributes;
  return it;
}
